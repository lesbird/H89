#pragma once

#include "cpu/Z80.h"
#include "devices/Ins8250.h"
#include "terminal/Crtc6845.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace h89 {

// H19 raster: 80 columns x 8 pixels, 25 rows x 10 scanlines.
constexpr int kScreenWidth = 640;
constexpr int kScreenHeight = 250;

// One luminance byte per pixel (0 = dark, 255 = full phosphor).
struct Framebuffer {
    std::vector<uint8_t> pixels = std::vector<uint8_t>(kScreenWidth * kScreenHeight);
};

// Keys that don't produce a plain ASCII character.
enum class H19Key : uint8_t {
    F1, F2, F3, F4, F5, Erase, Blue, Red, White,
    Keypad0, Keypad1, Keypad2, Keypad3, Keypad4,
    Keypad5, Keypad6, Keypad7, Keypad8, Keypad9,
    KeypadDot, KeypadEnter, Scroll,
};

// SW401 (port 00h): serial port setup.
namespace sw401 {
constexpr uint8_t kBaudMask = 0x0F;   // 1 = 110 ... 12 = 9600, 13 = 19200
constexpr uint8_t kBaud9600 = 0x0C;
constexpr uint8_t kParity = 0x10;     // parity enabled (then 7 data bits)
constexpr uint8_t kEvenParity = 0x20;
constexpr uint8_t kStickParity = 0x40;
constexpr uint8_t kFullDuplex = 0x80; // 0 = half duplex: keys echo locally
} // namespace sw401

// SW402 (port 20h): terminal modes.
namespace sw402 {
constexpr uint8_t kBlockCursor = 0x01;
constexpr uint8_t kNoKeyClick = 0x02;
constexpr uint8_t kWrapAtEol = 0x04;
constexpr uint8_t kAutoLf = 0x08;   // LF after received CR
constexpr uint8_t kAutoCr = 0x10;   // CR after received LF
constexpr uint8_t kAnsiMode = 0x20; // ANSI instead of Heath escape sequences
constexpr uint8_t kKeypadShifted = 0x40;
constexpr uint8_t k50Hz = 0x80;
} // namespace sw402

struct H19Config {
    std::vector<uint8_t> rom;  // 444-46 firmware, 4K at 0000h
    std::vector<uint8_t> font; // 444-29 character generator, 16 bytes per glyph
    uint8_t sw401 = sw401::kBaud9600 | sw401::kFullDuplex;
    uint8_t sw402 = 0;
};

// The H19 terminal board: its own Z80 running the terminal firmware, 2K of
// video RAM shown through a 6845 CRTC, an 8250 serial port to the computer,
// and the keyboard encoder.
class H19 : private Z80Bus {
public:
    static constexpr int kCpuHz = 2'048'000;      // 12.288 MHz / 6
    static constexpr int kAceXtalHz = 3'072'000;  // 12.288 MHz / 4
    static constexpr int kCharClockHz = 1'536'000; // 12.288 MHz / 8 dots

    explicit H19(H19Config config);

    void reset();
    // Executes one instruction and advances the devices; returns cycles.
    int step();

    // Serial line to the computer.
    std::function<void(uint8_t)> serialOut;
    void serialIn(uint8_t byte) { ace_.receive(byte); }

    // Keyboard. Keystrokes are queued and handed to the firmware one at a
    // time, like the keyboard encoder's strobe.
    void typeChar(uint8_t ascii); // printable and control characters
    void pressKey(H19Key key, bool shift = false);
    void setBreak(bool down) { breakDown_ = down; }
    void setOffline(bool offline) { offline_ = offline; }
    bool keyboardIdle() const { return keyQueue_.empty() && !keyLatched_; }

    std::function<void()> onBell;
    std::function<void()> onKeyClick;

    const Framebuffer& framebuffer() const { return fb_; }
    uint64_t frameCount() const { return frames_; }
    // The displayed screen as text, one line per row (reverse video and
    // graphics characters are shown as their base codes).
    std::string screenText() const;

    void saveState(StateWriter& w) const;
    void loadState(StateReader& r);
    // Cursor position on the displayed screen; false if it is off screen.
    bool cursorPosition(int& row, int& col) const;

    Z80& cpu() { return cpu_; }
    const Crtc6845& crtc() const { return crtc_; }

private:
    struct KeyEvent {
        uint8_t code;
        uint8_t flags; // modifier bits as the firmware sees them
    };

    uint8_t memRead(uint16_t addr) override;
    void memWrite(uint16_t addr, uint8_t value) override;
    uint8_t ioRead(uint16_t port) override;
    void ioWrite(uint16_t port, uint8_t value) override;

    uint8_t keyboardFlags() const;
    void tickKeyboard(int cycles);
    void tickVideo(int cycles);
    void renderFrame();
    int frameCycles() const;
    int displayEndCycle() const;

    H19Config config_;
    Z80 cpu_;
    Ins8250 ace_{kCpuHz, kAceXtalHz};
    Crtc6845 crtc_;
    std::array<uint8_t, 1024> ram_{};
    std::array<uint8_t, 2048> vram_{};

    // Keyboard encoder
    std::deque<KeyEvent> keyQueue_;
    bool keyLatched_ = false;
    KeyEvent latched_{0, 0};
    uint8_t lastKeyFlags_ = 0;
    int keyGapCycles_ = 0;
    bool breakDown_ = false;
    bool offline_ = false;

    // Video timing
    int frameCycle_ = 0;
    bool inDisplay_ = true;
    bool nmiArmed_ = false;
    Framebuffer fb_;
    uint64_t frames_ = 0;
};

} // namespace h89
