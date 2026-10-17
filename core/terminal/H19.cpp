#include "H19.h"

#include <algorithm>
#include <utility>

namespace h89 {

namespace {

// Keyboard flag bits as the firmware sees them after it XORs port A0h with
// F6h (bits 1, 2, 4-7 are active low on the bus).
constexpr uint8_t KF_SHIFT = 0x01;
constexpr uint8_t KF_BREAK = 0x04;
constexpr uint8_t KF_ONLINE = 0x08;
constexpr uint8_t KF_CTRL = 0x10;
constexpr uint8_t KF_LSHIFT = 0x20;
constexpr uint8_t KF_KEY_READY = 0x80;
constexpr uint8_t kFlagInvert = 0xF6;

// Codes from the 444-37 keyboard ROM for keys without an ASCII meaning; the
// firmware turns them into escape sequences (F1 = ESC S, ...).
constexpr uint8_t kSpecialKeyCode[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, // F1-F5
    0x05,                         // ERASE
    0x06, 0x07, 0x0E,             // BLUE, RED, WHITE
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, // keypad 0-9
    0x1A, 0x1C,                   // keypad . and ENTER
    0x1F,                         // SCROLL
};

// Minimum gap between keystrokes delivered from the queue, so pasted text
// doesn't overflow the firmware's 8-entry key buffer.
constexpr int kKeyGapCycles = H19::kCpuHz / 100;

} // namespace

H19::H19(H19Config config) : config_(std::move(config)), cpu_(*this) {
    ace_.onTransmit = [this](uint8_t b) {
        if (serialOut) serialOut(b);
    };
    reset();
}

void H19::reset() {
    cpu_.reset();
    ace_.reset();
    ace_.setModemInputs(0xB0); // CTS, DSR, DCD from the computer
    crtc_.reset();
    ram_.fill(0);
    // Video RAM powers up with junk on real hardware; spaces look tidier
    // until the firmware clears it.
    vram_.fill(0x20);
    keyQueue_.clear();
    keyLatched_ = false;
    lastKeyFlags_ = 0;
    keyGapCycles_ = 0;
    frameCycle_ = 0;
    inDisplay_ = true;
    nmiArmed_ = false;
    frames_ = 0;
    std::fill(fb_.pixels.begin(), fb_.pixels.end(), 0);
    scrollback_.clear();
    haveLastScreen_ = false;
    heldFrames_ = 0;
    viewOffset_ = 0;
}

uint8_t H19::memRead(uint16_t addr) {
    if (addr < 0x4000) {
        uint16_t off = addr & 0x0FFF;
        return off < config_.rom.size() ? config_.rom[off] : 0xFF;
    }
    if (addr < 0x8000) return ram_[addr & 0x3FF];
    if (addr < 0xC000) return 0xFF;
    return vram_[addr & 0x7FF];
}

void H19::memWrite(uint16_t addr, uint8_t value) {
    if (addr >= 0xC000) {
        // Past the 25th line: the firmware is moving the 25th line down a row
        // to scroll, and the move wraps onto the start of the top row.
        if (((addr - crtc_.startAddress()) & 0x7FF) >= 25 * kScreenColumns) heldFrames_ = 3;
        vram_[addr & 0x7FF] = value;
    }
    else if (addr >= 0x4000 && addr < 0x8000) ram_[addr & 0x3FF] = value;
}

uint8_t H19::keyboardFlags() const {
    uint8_t f = lastKeyFlags_;
    if (breakDown_) f |= KF_BREAK;
    if (!offline_) f |= KF_ONLINE;
    if (keyLatched_) f |= KF_KEY_READY;
    return f ^ kFlagInvert;
}

uint8_t H19::ioRead(uint16_t port) {
    uint8_t p = port & 0xFF;
    switch (p & 0xE0) {
    case 0x00: return config_.sw401;
    case 0x20: return config_.sw402;
    case 0x40: return ace_.read(p & 7);
    case 0x60: return (p & 0x05) == 0x01 ? crtc_.readData() : 0xFF;
    case 0x80: // key data; reading it clears the keyboard interrupt
        if (keyLatched_) {
            keyLatched_ = false;
            keyGapCycles_ = kKeyGapCycles;
        }
        return latched_.code;
    case 0xA0: return keyboardFlags();
    default: return 0xFF;
    }
}

void H19::ioWrite(uint16_t port, uint8_t value) {
    uint8_t p = port & 0xFF;
    switch (p & 0xE0) {
    case 0x40:
        ace_.write(p & 7, value);
        break;
    case 0x60:
        if (p & 0x04) {
            // Request an NMI at the next vertical blank; the handler copies
            // the cursor and start-address shadows into the 6845 then.
            nmiArmed_ = true;
        } else if (p & 0x01) {
            crtc_.writeData(value);
        } else {
            crtc_.writeAddress(value);
        }
        break;
    case 0xC0:
        if (onKeyClick) onKeyClick();
        break;
    case 0xE0:
        if (onBell) onBell();
        break;
    default:
        break;
    }
}

void H19::setLineRate(int baud) {
    // 10 bits a character: start, 8 data (or 7 + parity), stop.
    ace_.setFixedCharCycles(baud > 0 ? std::max(1, static_cast<int>(int64_t(10) * kCpuHz / baud)) : 0);
}

int H19::run(int cycles) {
    int done = 0;
    while (done < cycles) done += step();
    return done;
}

bool H19::takeTypedFlowKey(uint8_t code) {
    int& n = code == 0x13 ? typedXoff_ : typedXon_;
    if (n == 0) return false;
    --n;
    return true;
}

void H19::typeChar(uint8_t ascii) {
    setViewOffset(0);
    ascii &= 0x7F;
    if (ascii == 0x13) ++typedXoff_;
    if (ascii == 0x11) ++typedXon_;
    switch (ascii) {
    case 0x08: case 0x09: case 0x0A: case 0x0D: case 0x1B: case 0x7F:
        // Keys with their own position on the keyboard
        keyQueue_.push_back({ascii, 0});
        return;
    default:
        break;
    }
    if (ascii < 0x20) {
        // CTRL+key: the keyboard ROM sets bit 7 and the firmware masks the
        // character down to a control code.
        keyQueue_.push_back({static_cast<uint8_t>((ascii | 0x40) | 0x80), KF_CTRL});
        return;
    }
    keyQueue_.push_back({ascii, 0});
}

void H19::pressKey(H19Key key, bool shift) {
    setViewOffset(0);
    uint8_t flags = shift ? (KF_SHIFT | KF_LSHIFT) : 0;
    keyQueue_.push_back({kSpecialKeyCode[static_cast<int>(key)], flags});
}

void H19::tickKeyboard(int cycles) {
    if (keyLatched_) return;
    if (keyGapCycles_ > 0) keyGapCycles_ -= cycles;
    if (keyGapCycles_ <= 0 && !keyQueue_.empty()) {
        latched_ = keyQueue_.front();
        keyQueue_.pop_front();
        lastKeyFlags_ = latched_.flags;
        keyLatched_ = true;
    }
}

int H19::frameCycles() const {
    int64_t chars = int64_t(crtc_.horizontalTotal()) * crtc_.totalScanLines();
    int cycles = static_cast<int>(chars * kCpuHz / kCharClockHz);
    return std::max(cycles, 10000); // guard against an unprogrammed CRTC
}

int H19::displayEndCycle() const {
    int64_t chars = int64_t(crtc_.horizontalTotal()) * crtc_.verticalDisplayed() *
                    crtc_.scanLinesPerRow();
    return static_cast<int>(chars * kCpuHz / kCharClockHz);
}

void H19::tickVideo(int cycles) {
    frameCycle_ += cycles;
    if (inDisplay_ && frameCycle_ >= displayEndCycle()) {
        inDisplay_ = false;
        trackScroll();
        renderFrame();
        ++frames_;
        if (nmiArmed_) {
            nmiArmed_ = false;
            cpu_.triggerNmi();
        }
    }
    int total = frameCycles();
    if (frameCycle_ >= total) {
        frameCycle_ -= total;
        inDisplay_ = true;
    }
}

int H19::step() {
    int t = cpu_.step();
    ace_.tick(t);
    tickKeyboard(t);
    tickVideo(t);
    cpu_.setIrq(ace_.irq() || keyLatched_);
    return t;
}

void H19::trackScroll() {
    const int cols = crtc_.horizontalDisplayed();
    if (cols != kScreenColumns) { // firmware not running (yet)
        haveLastScreen_ = false;
        return;
    }
    const uint16_t start = crtc_.startAddress();
    // What the top row held before the firmware began moving the 25th line
    // over the start of it.
    Line top;
    bool holdTop = false;
    if (haveLastScreen_) {
        int moved = (start - lastStart_) & 0x7FF; // 2K video RAM wraps
        int rows = moved / cols;
        if (moved % cols == 0 && rows >= 1 && rows <= kScrollingRows) {
            for (int i = 0; i < rows; ++i) scrollback_.push_back(lastScreen_[i]);
            while (scrollback_.size() > size_t(kScrollbackLines)) scrollback_.pop_front();
            if (viewOffset_ > 0) viewOffset_ = std::min(viewOffset_ + rows, scrollbackLines());
            if (rows < kScrollingRows) top = lastScreen_[rows], holdTop = true;
        } else if (moved == 0) {
            top = lastScreen_[0], holdTop = true;
        }
    }
    for (int row = 0; row < kScrollingRows; ++row)
        for (int col = 0; col < cols; ++col) lastScreen_[row][col] = vram_[(start + row * cols + col) & 0x7FF];
    if (heldFrames_ > 0) {
        --heldFrames_;
        constexpr int kOverlap = (kScrollingRows + 2) * kScreenColumns - 2048; // 32 characters
        if (holdTop) std::copy_n(top.begin(), kOverlap, lastScreen_[0].begin());
    }
    lastStart_ = start;
    haveLastScreen_ = true;
}

void H19::setViewOffset(int lines) {
    lines = std::clamp(lines, 0, scrollbackLines());
    if (lines == viewOffset_) return;
    viewOffset_ = lines;
    renderFrame();
}

void H19::renderFrame() {
    auto& px = fb_.pixels;
    std::fill(px.begin(), px.end(), 0);
    if (config_.font.empty()) return;

    const int cols = std::min(crtc_.horizontalDisplayed(), kScreenColumns);
    const int lines = crtc_.scanLinesPerRow();
    const int rows = std::min(crtc_.verticalDisplayed(), kScreenHeight / std::max(lines, 1));
    const uint16_t start = crtc_.startAddress();
    const uint16_t cursor = crtc_.cursorAddress();

    bool cursorOn;
    switch (crtc_.cursorMode()) {
    case Crtc6845::CursorMode::Steady: cursorOn = true; break;
    case Crtc6845::CursorMode::Hidden: cursorOn = false; break;
    case Crtc6845::CursorMode::Blink16: cursorOn = (frames_ / 8) % 2 == 0; break;
    default: cursorOn = (frames_ / 16) % 2 == 0; break;
    }

    for (int row = 0; row < rows; ++row) {
        // Scrolled back, the top rows come from the scrollback.
        const int src = row < kScrollingRows ? row - viewOffset_ : row;
        const Line* old = src < 0 ? &scrollback_[scrollback_.size() + src] : nullptr;
        for (int col = 0; col < cols; ++col) {
            uint16_t ma = (start + src * crtc_.horizontalDisplayed() + col) & 0x3FFF;
            uint8_t ch = old ? (*old)[col] : vram_[ma & 0x7FF];
            bool isCursor = !old && cursorOn && ma == cursor;
            for (int ra = 0; ra < lines; ++ra) {
                int y = row * lines + ra;
                if (y >= kScreenHeight) break;
                size_t glyph = size_t(ch & 0x7F) * 16 + (ra & 0x0F);
                uint8_t bits = glyph < config_.font.size() ? config_.font[glyph] : 0;
                if (ch & 0x80) bits = ~bits; // reverse video
                if (isCursor && ra >= crtc_.cursorStart() && ra <= crtc_.cursorEnd()) bits = ~bits;
                uint8_t* out = &px[y * kScreenWidth + col * 8];
                for (int b = 0; b < 8; ++b) out[b] = (bits & (0x80 >> b)) ? 255 : 0;
            }
        }
    }
}

bool H19::cursorPosition(int& row, int& col) const {
    const int cols = crtc_.horizontalDisplayed();
    if (cols <= 0) return false;
    int offset = (crtc_.cursorAddress() - crtc_.startAddress()) & 0x7FF; // 2K video RAM wraps
    row = offset / cols;
    col = offset % cols;
    return row < crtc_.verticalDisplayed();
}

std::string H19::screenText() const {
    std::string text;
    const int cols = crtc_.horizontalDisplayed();
    const int rows = crtc_.verticalDisplayed();
    for (int row = 0; row < rows; ++row) {
        const int src = row < kScrollingRows ? row - viewOffset_ : row;
        const Line* old = src < 0 ? &scrollback_[scrollback_.size() + src] : nullptr;
        std::string line;
        for (int col = 0; col < cols; ++col) {
            uint16_t ma = (crtc_.startAddress() + src * cols + col) & 0x3FFF;
            uint8_t ch = (old && col < kScreenColumns ? (*old)[col] : vram_[ma & 0x7FF]) & 0x7F;
            line += (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : ' ';
        }
        line.erase(line.find_last_not_of(' ') + 1);
        text += line + '\n';
    }
    return text;
}

void H19::saveState(StateWriter& w) const {
    w.begin("H19 ");
    cpu_.saveState(w);
    ace_.saveState(w);
    crtc_.saveState(w);
    w.bytes(ram_);
    w.bytes(vram_);
    w.u32(static_cast<uint32_t>(keyQueue_.size()));
    for (const KeyEvent& k : keyQueue_) w.u8(k.code), w.u8(k.flags);
    w.boolean(keyLatched_), w.u8(latched_.code), w.u8(latched_.flags);
    w.u8(lastKeyFlags_), w.i32(keyGapCycles_), w.boolean(breakDown_), w.boolean(offline_);
    w.i32(frameCycle_), w.boolean(inDisplay_), w.boolean(nmiArmed_);
    w.bytes(fb_.pixels);
    w.u64(frames_);
    w.end();
}

void H19::loadState(StateReader& r) {
    if (!r.begin("H19 ")) return;
    cpu_.loadState(r);
    ace_.loadState(r);
    crtc_.loadState(r);
    r.bytes(ram_);
    r.bytes(vram_);
    keyQueue_.clear();
    for (uint32_t n = r.u32(); n > 0 && r.ok(); --n) {
        uint8_t code = r.u8();
        keyQueue_.push_back({code, r.u8()});
    }
    keyLatched_ = r.boolean(), latched_.code = r.u8(), latched_.flags = r.u8();
    lastKeyFlags_ = r.u8(), keyGapCycles_ = r.i32(), breakDown_ = r.boolean(), offline_ = r.boolean();
    frameCycle_ = r.i32(), inDisplay_ = r.boolean(), nmiArmed_ = r.boolean();
    std::vector<uint8_t> pixels = r.bytes();
    if (pixels.size() == fb_.pixels.size()) fb_.pixels = std::move(pixels);
    frames_ = r.u64();
    r.end();
    haveLastScreen_ = false; // the start address jumped; that isn't scrolling
    viewOffset_ = 0;
}

} // namespace h89
