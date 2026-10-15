#include "FileTransfer.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>

namespace h89 {

namespace fs = std::filesystem;
using Clock = FileTransfer::Clock;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

constexpr uint8_t SOH = 0x01, STX = 0x02, EOT = 0x04, ACK = 0x06, BS = 0x08, NAK = 0x15, CAN = 0x18;
constexpr uint8_t XON = 0x11, XOFF = 0x13, CPMEOF = 0x1A;

bool readFile(const std::string& path, std::vector<uint8_t>& out, std::string* error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }
    out.clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    bool ok = !std::ferror(f);
    std::fclose(f);
    if (!ok && error) *error = "cannot read " + path;
    return ok;
}

std::string baseName(const std::string& path) {
    return fs::path(path).filename().string();
}

std::string withCommas(uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

// CRC-16/XMODEM (CCITT polynomial, initial value 0), used by XMODEM and by
// ZMODEM's 16-bit frames.
uint16_t crc16(const uint8_t* p, size_t n, uint16_t crc = 0) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= static_cast<uint16_t>(p[i] << 8);
        for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

// CRC-32 (IEEE, as in zip), for ZMODEM's 32-bit frames: start with
// 0xFFFFFFFF and invert at the end.
uint32_t crc32Update(uint32_t crc, const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

} // namespace

FileTransfer::~FileTransfer() = default;

void FileTransfer::cancel(std::vector<uint8_t>& out) {
    if (finished()) return;
    // Enough CANs for XMODEM and ZMODEM, then backspaces to tidy the line.
    out.insert(out.end(), 8, CAN);
    out.insert(out.end(), 8, BS);
    state_ = State::Cancelled;
    message_ = "Cancelled";
}

void FileTransfer::setSavePath(const std::string&) {}

std::string FileTransfer::status() const {
    switch (state_) {
    case State::Failed:
    case State::Cancelled: return message_;
    case State::NeedsSavePath: return "Choose where to save " + offeredName_;
    case State::Done:
        return "Done: " + withCommas(done_) + " bytes" + (message_.empty() ? "" : " (" + message_ + ")");
    case State::Running: break;
    }
    return total_ ? withCommas(done_) + " of " + withCommas(total_) + " bytes" : withCommas(done_) + " bytes";
}

std::string cpmFileName(const std::string& hostName) {
    std::string name = baseName(hostName), stem = name, ext;
    size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) stem = name.substr(0, dot), ext = name.substr(dot + 1);
    auto clean = [](const std::string& s, size_t max) {
        std::string out;
        for (unsigned char c : s) {
            if (out.size() == max) break;
            if (std::isalnum(c) || std::strchr("$#&@!%'()-{}~_^`", c)) out += static_cast<char>(std::toupper(c));
        }
        return out;
    };
    stem = clean(stem, 8);
    ext = clean(ext, 3);
    if (stem.empty()) stem = "FILE";
    return ext.empty() ? stem : stem + "." + ext;
}

// --- Text -----------------------------------------------------------------------

namespace {

class TextSender : public FileTransfer {
public:
    TextSender(const std::vector<uint8_t>& text, const std::string& name, const TextSendOptions& o) : options_(o) {
        title_ = "Sending " + name + " as text";
        using LE = TextSendOptions::LineEnding;
        auto endLine = [&] {
            switch (o.lineEnding) {
            case LE::CR: data_.push_back('\r'); break;
            case LE::CRLF: data_.push_back('\r'), data_.push_back('\n'); break;
            case LE::LF: data_.push_back('\n'); break;
            case LE::AsIs: break;
            }
            lineEnds_.push_back(data_.size() - 1);
        };
        for (size_t i = 0; i < text.size(); ++i) {
            uint8_t c = text[i];
            if (c == CPMEOF) break;
            bool crlf = c == '\r' && i + 1 < text.size() && text[i + 1] == '\n';
            if (o.lineEnding == LE::AsIs) {
                data_.push_back(c);
                if (c == '\n' || (c == '\r' && !crlf)) lineEnds_.push_back(data_.size() - 1);
            } else if (c == '\r' || c == '\n') {
                if (crlf) ++i;
                endLine();
            } else {
                data_.push_back(c);
            }
        }
        total_ = data_.size();
    }

    bool echoesToScreen() const override { return true; }

    void received(const uint8_t* data, size_t size, Clock::time_point) override {
        for (size_t i = 0; i < size; ++i) {
            if ((data[i] & 0x7F) == XOFF) paused_ = true;
            if ((data[i] & 0x7F) == XON) paused_ = false;
        }
    }

    void poll(Clock::time_point now, std::vector<uint8_t>& out, size_t room) override {
        if (finished()) return;
        while (room > 0 && pos_ < data_.size() && !paused_ && now >= nextAt_) {
            out.push_back(data_[pos_]);
            int delay = options_.charDelayMs;
            if (lineAt_ < lineEnds_.size() && lineEnds_[lineAt_] == pos_) {
                delay += options_.lineDelayMs;
                ++lineAt_;
            }
            ++pos_;
            --room;
            if (delay > 0) nextAt_ = now + milliseconds(delay);
        }
        done_ = pos_;
        if (pos_ == data_.size()) finish();
    }

private:
    TextSendOptions options_;
    std::vector<uint8_t> data_;
    std::vector<size_t> lineEnds_; // positions of the last byte of each line ending
    size_t lineAt_ = 0;
    size_t pos_ = 0;
    bool paused_ = false;
    Clock::time_point nextAt_{};
};

// --- XMODEM -----------------------------------------------------------------------

class XmodemSender : public FileTransfer {
public:
    XmodemSender(std::vector<uint8_t> data, const std::string& name, bool oneK) : data_(std::move(data)), oneK_(oneK) {
        title_ = "Uploading " + name + " with XMODEM" + (oneK ? "-1K" : "");
        total_ = data_.size();
    }

    void received(const uint8_t* data, size_t size, Clock::time_point now) override {
        for (size_t i = 0; i < size && !finished(); ++i) {
            uint8_t c = data[i];
            if (c == CAN) {
                if (++cans_ >= 2) fail("Cancelled by the computer");
                continue;
            }
            cans_ = 0;
            switch (state_x_) {
            case S::WaitStart:
                if (c == 'C' || c == NAK) {
                    crc_ = c == 'C';
                    if (data_.empty()) sendEot(now);
                    else sendBlock(now, true);
                }
                break;
            case S::WaitAck:
                // A late 'C' from the receiver's start-up is ignored: answering
                // it would resend block 1 and mistake its ACK for block 2's.
                if (c == ACK) {
                    offset_ += blockLen_;
                    done_ = std::min<uint64_t>(offset_, data_.size());
                    ++block_;
                    if (offset_ >= data_.size()) sendEot(now);
                    else sendBlock(now, true);
                } else if (c == NAK) {
                    retry(now);
                }
                break;
            case S::WaitEotAck:
                if (c == ACK) finish();
                else if (c == NAK) retry(now);
                break;
            }
        }
    }

    void poll(Clock::time_point now, std::vector<uint8_t>& out, size_t room) override {
        size_t n = std::min(room, pending_.size());
        out.insert(out.end(), pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
        if (finished()) return;
        if (state_x_ == S::WaitStart && startBy_ == Clock::time_point{}) startBy_ = now + seconds(60);
        if (!pending_.empty()) {
            deadline_ = now + seconds(10); // the timeout runs once it's all sent
            return;
        }
        if (state_x_ == S::WaitStart && now > startBy_) fail("The computer never started receiving");
        else if (state_x_ != S::WaitStart && now > deadline_) retry(now);
    }

    void cancel(std::vector<uint8_t>& out) override {
        pending_.clear(); // the rest of a block would only confuse the receiver
        FileTransfer::cancel(out);
    }

private:
    enum class S { WaitStart, WaitAck, WaitEotAck };

    void sendBlock(Clock::time_point now, bool fresh) {
        if (fresh) retries_ = 0;
        size_t left = data_.size() - offset_;
        blockLen_ = oneK_ && crc_ && left > 128 ? 1024 : 128;
        pending_.clear();
        pending_.push_back(blockLen_ == 1024 ? STX : SOH);
        pending_.push_back(static_cast<uint8_t>(block_));
        pending_.push_back(static_cast<uint8_t>(~block_));
        std::vector<uint8_t> block(blockLen_, CPMEOF);
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset_), std::min(left, blockLen_), block.begin());
        pending_.insert(pending_.end(), block.begin(), block.end());
        if (crc_) {
            uint16_t c = crc16(block.data(), block.size());
            pending_.push_back(static_cast<uint8_t>(c >> 8));
            pending_.push_back(static_cast<uint8_t>(c));
        } else {
            uint8_t sum = 0;
            for (uint8_t b : block) sum = static_cast<uint8_t>(sum + b);
            pending_.push_back(sum);
        }
        state_x_ = S::WaitAck;
        deadline_ = now + seconds(10);
    }

    void sendEot(Clock::time_point now) {
        pending_.assign(1, EOT);
        state_x_ = S::WaitEotAck;
        deadline_ = now + seconds(10);
    }

    void retry(Clock::time_point now) {
        if (++retries_ > 10) return fail("Too many errors");
        if (state_x_ == S::WaitEotAck) sendEot(now);
        else sendBlock(now, false);
    }

    std::vector<uint8_t> data_;
    bool oneK_;
    bool crc_ = false;
    S state_x_ = S::WaitStart;
    size_t offset_ = 0, blockLen_ = 128;
    unsigned block_ = 1;
    int retries_ = 0, cans_ = 0;
    std::vector<uint8_t> pending_;
    Clock::time_point deadline_{}, startBy_{};
};

class XmodemReceiver : public FileTransfer {
public:
    XmodemReceiver(std::FILE* file, const std::string& path) : file_(file), path_(path) {
        title_ = "Downloading " + baseName(path) + " with XMODEM";
    }
    ~XmodemReceiver() override { closeFile(state_ != State::Done); }

    void received(const uint8_t* data, size_t size, Clock::time_point now) override {
        for (size_t i = 0; i < size && !finished(); ++i) {
            uint8_t c = data[i];
            lastByte_ = now;
            if (purging_) {
                // Two CANs while waiting for the line to go quiet: the sender
                // gave up.
                if (c == CAN && ++cans_ >= 2) fail("Cancelled by the computer");
                else if (c != CAN) cans_ = 0;
                continue;
            }
            if (buf_.empty()) {
                if (c == SOH || c == STX) {
                    started_ = true;
                    need_ = 3 + (c == STX ? 1024u : 128u) + (crc_ ? 2u : 1u);
                    buf_.push_back(c);
                } else if (c == EOT) { // an empty file sends nothing else
                    pending_.push_back(ACK);
                    closeFile(false);
                    saved_.push_back(path_);
                    finish();
                } else if (c == CAN) {
                    if (++cans_ >= 2) fail("Cancelled by the computer");
                } else if (started_) {
                    cans_ = 0;
                    purge(); // a block's start was garbled
                }
                continue;
            }
            buf_.push_back(c);
            if (buf_.size() == need_) block();
        }
    }

    void poll(Clock::time_point now, std::vector<uint8_t>& out, size_t room) override {
        if (!finished()) {
            if (!started_) {
                // Ask for CRCs a few times, then settle for checksums.
                if (now >= nextPing_) {
                    if (++pings_ > 20) {
                        fail("The computer never started sending");
                    } else {
                        crc_ = pings_ <= 4;
                        pending_.push_back(crc_ ? 'C' : NAK);
                        nextPing_ = now + seconds(3);
                    }
                }
            } else if (purging_ && now - lastByte_ > seconds(1)) {
                // The line has gone quiet: ask for the block again.
                purging_ = false;
                cans_ = 0;
                nak();
            } else if (!buf_.empty() && now - lastByte_ > seconds(1)) {
                // A block stopped short. If it ends in CANs, the sender gave up.
                size_t from = buf_.size() > 16 ? buf_.size() - 16 : 1;
                bool cancelled = false;
                for (size_t i = from; i + 1 < buf_.size(); ++i) cancelled |= buf_[i] == CAN && buf_[i + 1] == CAN;
                buf_.clear();
                if (cancelled) fail("Cancelled by the computer");
                else nak();
            } else if (!purging_ && buf_.empty() && now - lastByte_ > seconds(10)) {
                lastByte_ = now;
                nak();
            }
        }
        size_t n = std::min(room, pending_.size());
        out.insert(out.end(), pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
        if (finished() && state_ != State::Done) closeFile(true);
    }

    void cancel(std::vector<uint8_t>& out) override {
        FileTransfer::cancel(out);
        closeFile(true);
    }

private:
    // Ignores the line until it goes quiet, then NAKs once, so one garbled
    // block costs one resend.
    void purge() {
        buf_.clear();
        purging_ = true;
    }

    void block() {
        const uint8_t* b = buf_.data();
        size_t len = buf_[0] == STX ? 1024 : 128;
        const uint8_t* payload = b + 3;
        bool good = (b[1] ^ b[2]) == 0xFF;
        if (good && crc_) {
            good = crc16(payload, len) == ((b[3 + len] << 8) | b[4 + len]);
        } else if (good) {
            uint8_t sum = 0;
            for (size_t i = 0; i < len; ++i) sum = static_cast<uint8_t>(sum + payload[i]);
            good = sum == b[3 + len];
        }
        uint8_t number = b[1];
        if (!good) return purge();
        if (number == static_cast<uint8_t>(expected_)) {
            if (std::fwrite(payload, 1, len, file_) != len) {
                buf_.clear();
                pending_.insert(pending_.end(), 3, CAN);
                return fail("Cannot write " + path_);
            }
            done_ += len;
            ++expected_;
            errors_ = 0;
            pending_.push_back(ACK);
        } else if (number == static_cast<uint8_t>(expected_ - 1)) {
            pending_.push_back(ACK); // a repeat: our ACK was lost
        } else {
            pending_.insert(pending_.end(), 3, CAN);
            fail("Blocks arrived out of order");
        }
        buf_.clear();
    }

    void nak() {
        if (++errors_ > 10) {
            pending_.insert(pending_.end(), 3, CAN);
            return fail("Too many errors");
        }
        pending_.push_back(NAK);
    }

    void closeFile(bool remove) {
        if (!file_) return;
        std::fclose(file_);
        file_ = nullptr;
        if (remove) std::remove(path_.c_str()); // don't leave half a file
    }

    std::FILE* file_;
    std::string path_;
    bool started_ = false, crc_ = true, purging_ = false;
    int pings_ = 0, errors_ = 0, cans_ = 0;
    unsigned expected_ = 1;
    size_t need_ = 0;
    std::vector<uint8_t> buf_, pending_;
    Clock::time_point nextPing_{}, lastByte_{};
};

// --- ZMODEM -----------------------------------------------------------------------

namespace z {
constexpr uint8_t ZPAD = '*', ZDLE = 0x18, ZBIN = 'A', ZHEX = 'B', ZBIN32 = 'C';
enum : uint8_t {
    ZRQINIT, ZRINIT, ZSINIT, ZACK, ZFILE, ZSKIP, ZNAK, ZABORT, ZFIN, ZRPOS, ZDATA, ZEOF, ZFERR, ZCRC,
    ZCHALLENGE, ZCOMPL, ZCAN, ZFREECNT, ZCOMMAND, ZSTDERR,
};
constexpr uint8_t ZCRCE = 'h', ZCRCG = 'i', ZCRCQ = 'j', ZCRCW = 'k', ZRUB0 = 'l', ZRUB1 = 'm';
constexpr uint8_t CANFDX = 0x01, CANOVIO = 0x02, CANFC32 = 0x20, ESCCTL = 0x40;
} // namespace z

// A header: its type and four bytes, which hold a file position (ZP0 is the
// low byte) or flags (ZF0 is the last byte).
struct ZHeader {
    uint8_t type = 0;
    uint8_t p[4] = {};
    uint32_t pos() const { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
    static ZHeader at(uint8_t type, uint32_t pos) {
        ZHeader h;
        h.type = type;
        for (int i = 0; i < 4; ++i) h.p[i] = static_cast<uint8_t>(pos >> (8 * i));
        return h;
    }
};

// Builds ZMODEM frames with ZDLE escaping.
class ZWriter {
public:
    bool escapeControls = false;

    void hexHeader(std::vector<uint8_t>& out, const ZHeader& h) {
        uint8_t b[5] = {h.type, h.p[0], h.p[1], h.p[2], h.p[3]};
        uint16_t crc = crc16(b, 5);
        static const char* hex = "0123456789abcdef";
        out.insert(out.end(), {z::ZPAD, z::ZPAD, z::ZDLE, z::ZHEX});
        auto put = [&](uint8_t v) { out.push_back(static_cast<uint8_t>(hex[v >> 4])), out.push_back(static_cast<uint8_t>(hex[v & 15])); };
        for (uint8_t v : b) put(v);
        put(static_cast<uint8_t>(crc >> 8));
        put(static_cast<uint8_t>(crc));
        out.push_back('\r');
        out.push_back(0x8A);
        if (h.type != z::ZFIN && h.type != z::ZACK) out.push_back(XON);
        last_ = 0;
    }

    void binHeader(std::vector<uint8_t>& out, const ZHeader& h, bool crc32) {
        uint8_t b[5] = {h.type, h.p[0], h.p[1], h.p[2], h.p[3]};
        out.push_back(z::ZPAD);
        out.push_back(z::ZDLE);
        out.push_back(crc32 ? z::ZBIN32 : z::ZBIN);
        for (uint8_t v : b) escape(out, v);
        trailer(out, b, 5, crc32);
    }

    // One data subpacket ending with `end` (ZCRCE, ZCRCG, ZCRCQ or ZCRCW).
    void data(std::vector<uint8_t>& out, const uint8_t* p, size_t n, uint8_t end, bool crc32) {
        for (size_t i = 0; i < n; ++i) escape(out, p[i]);
        out.push_back(z::ZDLE);
        out.push_back(end);
        std::vector<uint8_t> covered(p, p + n);
        covered.push_back(end);
        trailer(out, covered.data(), covered.size(), crc32);
        if (end == z::ZCRCW) out.push_back(XON);
    }

private:
    void trailer(std::vector<uint8_t>& out, const uint8_t* p, size_t n, bool crc32) {
        if (crc32) {
            uint32_t c = ~crc32Update(0xFFFFFFFFu, p, n);
            for (int i = 0; i < 4; ++i) escape(out, static_cast<uint8_t>(c >> (8 * i)));
        } else {
            uint16_t c = crc16(p, n);
            escape(out, static_cast<uint8_t>(c >> 8));
            escape(out, static_cast<uint8_t>(c));
        }
    }

    void escape(std::vector<uint8_t>& out, uint8_t c) {
        uint8_t c7 = c & 0x7F;
        bool need = c == z::ZDLE || c7 == 0x10 || c7 == XON || c7 == XOFF || (c7 == '\r' && (last_ & 0x7F) == '@') ||
                    (escapeControls && (c7 < 0x20 || c7 == 0x7F));
        if (need && c7 == 0x7F) {
            out.push_back(z::ZDLE);
            out.push_back(c == 0x7F ? z::ZRUB0 : z::ZRUB1);
        } else if (need) {
            out.push_back(z::ZDLE);
            out.push_back(c ^ 0x40);
        } else {
            out.push_back(c);
        }
        last_ = c;
    }

    uint8_t last_ = 0;
};

// Reads ZMODEM frames out of the byte stream.
class ZParser {
public:
    struct Event {
        enum Kind { Header, Data, Cancel } kind;
        ZHeader header;
        bool header32 = false;     // a ZBIN32 header: the data after it has 32-bit CRCs
        std::vector<uint8_t> data; // a subpacket
        uint8_t end = 0;           // its ZCRCx
        bool good = true;          // its CRC matched
    };

    // The next bytes are a data subpacket (after ZFILE, ZDATA or ZSINIT).
    void expectData(bool crc32) {
        mode_ = Mode::Data;
        crc32_ = crc32;
        buf_.clear();
        escaped_ = false;
    }
    void reset() { mode_ = Mode::Search, pad_ = 0; }

    template <typename F>
    void feed(uint8_t c, F&& onEvent) {
        // Five CANs (which are ZDLEs) in a row abort.
        if (c == z::ZDLE) {
            if (++cans_ >= 5) {
                cans_ = 0;
                reset();
                onEvent(Event{Event::Cancel, {}, false, {}, 0, true});
                return;
            }
        } else {
            cans_ = 0;
        }
        switch (mode_) {
        case Mode::Search: {
            uint8_t c7 = c & 0x7F;
            if (c7 == z::ZPAD) {
                pad_ = 1;
            } else if (pad_ >= 1 && c == z::ZDLE) {
                pad_ = 2;
            } else if (pad_ == 2) {
                pad_ = 0;
                buf_.clear();
                escaped_ = false;
                if (c7 == z::ZBIN || c7 == z::ZBIN32) mode_ = Mode::Bin, header32_ = c7 == z::ZBIN32;
                else if (c7 == z::ZHEX) mode_ = Mode::Hex, header32_ = false;
            } else {
                pad_ = 0;
            }
            break;
        }
        case Mode::Hex: {
            int v = hexValue(c & 0x7F);
            if (v < 0) {
                reset();
                break;
            }
            buf_.push_back(static_cast<uint8_t>(v));
            if (buf_.size() == 14) {
                uint8_t b[7];
                for (int i = 0; i < 7; ++i) b[i] = static_cast<uint8_t>(buf_[2 * i] << 4 | buf_[2 * i + 1]);
                reset();
                if (crc16(b, 5) == (b[5] << 8 | b[6])) onEvent(headerEvent(b));
            }
            break;
        }
        case Mode::Bin: {
            int v = unescape(c);
            if (v == kMore) break;
            if (v < 0 || v > 0xFF) {
                reset();
                break;
            }
            buf_.push_back(static_cast<uint8_t>(v));
            if (buf_.size() == (header32_ ? 9u : 7u)) {
                bool good;
                if (header32_) {
                    uint32_t want = buf_[5] | buf_[6] << 8 | buf_[7] << 16 | static_cast<uint32_t>(buf_[8]) << 24;
                    good = ~crc32Update(0xFFFFFFFFu, buf_.data(), 5) == want;
                } else {
                    good = crc16(buf_.data(), 5) == (buf_[5] << 8 | buf_[6]);
                }
                uint8_t b[5];
                std::copy_n(buf_.begin(), 5, b);
                reset();
                if (good) onEvent(headerEvent(b));
            }
            break;
        }
        case Mode::Data: {
            int v = unescape(c);
            if (v == kMore) break;
            if (v < 0) {
                reset();
                break;
            }
            if (v > 0xFF) { // the frame end
                end_ = static_cast<uint8_t>(v & 0xFF);
                crcBytes_.clear();
                mode_ = Mode::DataCrc;
            } else if (buf_.size() >= 8192) {
                reset();
            } else {
                buf_.push_back(static_cast<uint8_t>(v));
            }
            break;
        }
        case Mode::DataCrc: {
            int v = unescape(c);
            if (v == kMore) break;
            if (v < 0 || v > 0xFF) {
                reset();
                break;
            }
            crcBytes_.push_back(static_cast<uint8_t>(v));
            if (crcBytes_.size() < (crc32_ ? 4u : 2u)) break;
            std::vector<uint8_t> covered = buf_;
            covered.push_back(end_);
            bool good;
            if (crc32_) {
                uint32_t want = crcBytes_[0] | crcBytes_[1] << 8 | crcBytes_[2] << 16 |
                                static_cast<uint32_t>(crcBytes_[3]) << 24;
                good = ~crc32Update(0xFFFFFFFFu, covered.data(), covered.size()) == want;
            } else {
                good = crc16(covered.data(), covered.size()) == (crcBytes_[0] << 8 | crcBytes_[1]);
            }
            Event e{Event::Data, {}, crc32_, std::move(buf_), end_, good};
            buf_.clear();
            // More subpackets follow ZCRCG and ZCRCQ; a header follows the others.
            if (good && (end_ == z::ZCRCG || end_ == z::ZCRCQ)) mode_ = Mode::Data;
            else reset();
            onEvent(e);
            break;
        }
        }
    }

private:
    enum class Mode { Search, Hex, Bin, Data, DataCrc };
    static constexpr int kMore = -1, kBad = -2;

    Event headerEvent(const uint8_t* b) {
        Event e{Event::Header, {}, header32_, {}, 0, true};
        e.header.type = b[0];
        std::copy_n(b + 1, 4, e.header.p);
        return e;
    }

    static int hexValue(uint8_t c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    // A byte, a frame end (0x100 | ZCRCx), kMore, or kBad.
    int unescape(uint8_t c) {
        if (escaped_) {
            escaped_ = false;
            switch (c) {
            case z::ZCRCE: case z::ZCRCG: case z::ZCRCQ: case z::ZCRCW: return 0x100 | c;
            case z::ZRUB0: return 0x7F;
            case z::ZRUB1: return 0xFF;
            default: return (c & 0x60) == 0x40 ? c ^ 0x40 : kBad;
            }
        }
        if (c == z::ZDLE) {
            escaped_ = true;
            return kMore;
        }
        if ((c & 0x7F) == XON || (c & 0x7F) == XOFF) return kMore; // flow control, never data
        return c;
    }

    Mode mode_ = Mode::Search;
    int pad_ = 0, cans_ = 0;
    bool escaped_ = false, header32_ = false, crc32_ = false;
    uint8_t end_ = 0;
    std::vector<uint8_t> buf_, crcBytes_;
};

// Shared by both ends: the pending output and its pacing.
class ZmodemBase : public FileTransfer {
protected:
    void flush(std::vector<uint8_t>& out, size_t room) {
        size_t n = std::min(room, pending_.size());
        out.insert(out.end(), pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
    }
    void abortLine() {
        pending_.clear();
        pending_.insert(pending_.end(), 8, CAN);
        pending_.insert(pending_.end(), 8, BS);
    }

    ZWriter writer_;
    ZParser parser_;
    std::vector<uint8_t> pending_;
};

class ZmodemSender : public ZmodemBase {
public:
    explicit ZmodemSender(std::vector<std::string> paths) : paths_(std::move(paths)) {
        title_ = paths_.size() == 1 ? "Uploading " + cpmFileName(paths_[0]) + " with ZMODEM"
                                    : "Uploading " + std::to_string(paths_.size()) + " files with ZMODEM";
        std::error_code ec;
        for (const auto& p : paths_) {
            uintmax_t size = fs::file_size(p, ec);
            if (!ec) total_ += size;
        }
    }

    void received(const uint8_t* data, size_t size, Clock::time_point now) override {
        for (size_t i = 0; i < size && !finished(); ++i)
            parser_.feed(data[i], [&](const ZParser::Event& e) { event(e, now); });
    }

    void poll(Clock::time_point now, std::vector<uint8_t>& out, size_t room) override {
        if (finished()) return flush(out, room); // e.g. the closing "OO"
        if (s_ == S::Init) {
            writer_.hexHeader(pending_, ZHeader::at(z::ZRQINIT, 0));
            enter(S::WaitInit, now);
        }
        const size_t start = out.size();
        flush(out, room);
        // Stream data while there's room on the line.
        while (s_ == S::Streaming && pending_.empty() && out.size() - start < room) {
            nextSubpacket(now);
            flush(out, room - (out.size() - start));
        }
        done_ = completed_ + pos_;
        if (!pending_.empty()) {
            deadline_ = now + seconds(10); // waiting starts once it's all sent
            return;
        }
        if (now > deadline_ && s_ != S::Streaming) timeout(now);
    }

    void cancel(std::vector<uint8_t>& out) override {
        pending_.clear();
        FileTransfer::cancel(out);
    }

private:
    enum class S { Init, WaitInit, WaitFilePos, Streaming, WaitAck, WaitEof, WaitFin };

    void enter(S s, Clock::time_point now) {
        if (s != s_) tries_ = 0;
        s_ = s;
        deadline_ = now + seconds(10);
    }

    void header(uint8_t type, uint32_t pos) { writer_.binHeader(pending_, ZHeader::at(type, pos), crc32_); }

    void event(const ZParser::Event& e, Clock::time_point now) {
        if (e.kind == ZParser::Event::Cancel) return fail("Cancelled by the computer");
        if (e.kind != ZParser::Event::Header) return;
        const ZHeader& h = e.header;
        switch (h.type) {
        case z::ZRINIT:
            rxBuffer_ = h.p[0] | h.p[1] << 8;
            crc32_ = (h.p[3] & z::CANFC32) != 0;
            writer_.escapeControls = (h.p[3] & z::ESCCTL) != 0;
            if (s_ == S::WaitInit) startFile(now);
            else if (s_ == S::WaitEof) nextFile(now);
            break;
        case z::ZRPOS:
            if (s_ == S::WaitFilePos || s_ == S::Streaming || s_ == S::WaitAck || s_ == S::WaitEof) {
                // Give up after many errors in a row with no progress between.
                if (s_ != S::WaitFilePos) {
                    errors_ = h.pos() > lastErrorPos_ ? 1 : errors_ + 1;
                    lastErrorPos_ = h.pos();
                    if (errors_ > 20) return fail("Too many errors");
                }
                if (h.pos() > data_.size()) return fail("The computer asked for a position past the end");
                pending_.clear();
                pos_ = windowStart_ = h.pos();
                header(z::ZDATA, pos_);
                enter(S::Streaming, now);
            }
            break;
        case z::ZACK:
            if (s_ == S::WaitAck) {
                windowStart_ = pos_;
                header(z::ZDATA, pos_);
                enter(S::Streaming, now);
            }
            break;
        case z::ZSKIP:
            if (s_ == S::WaitFilePos || s_ == S::Streaming || s_ == S::WaitAck) nextFile(now, true);
            break;
        case z::ZNAK:
            timeout(now); // resend whatever we're waiting on
            break;
        case z::ZFIN:
            if (s_ == S::WaitFin) {
                pending_.push_back('O');
                pending_.push_back('O');
                finish();
            }
            break;
        case z::ZCRC: {
            uint32_t crc = ~crc32Update(0xFFFFFFFFu, data_.data(), data_.size());
            header(z::ZCRC, crc);
            break;
        }
        case z::ZABORT:
        case z::ZFERR:
        case z::ZCAN: fail("The computer stopped the transfer"); break;
        default: break;
        }
    }

    void startFile(Clock::time_point now) {
        if (index_ >= paths_.size()) return sendFin(now);
        std::string error;
        if (!readFile(paths_[index_], data_, &error)) {
            abortLine();
            return fail(error);
        }
        name_ = cpmFileName(paths_[index_]);
        if (paths_.size() > 1)
            title_ = "Uploading " + name_ + " with ZMODEM (" + std::to_string(index_ + 1) + " of " +
                     std::to_string(paths_.size()) + ")";
        pos_ = windowStart_ = 0;
        sendFileHeader();
        enter(S::WaitFilePos, now);
    }

    void sendFileHeader() {
        struct stat st {};
        long mtime = ::stat(paths_[index_].c_str(), &st) == 0 ? static_cast<long>(st.st_mtime) : 0;
        char info[96];
        std::snprintf(info, sizeof info, "%zu %lo 0 0 %zu %llu", data_.size(), mtime, paths_.size() - index_,
                      static_cast<unsigned long long>(total_ - completed_));
        std::vector<uint8_t> sub(name_.begin(), name_.end());
        sub.push_back(0);
        sub.insert(sub.end(), info, info + std::strlen(info));
        sub.push_back(0);
        header(z::ZFILE, 0);
        writer_.data(pending_, sub.data(), sub.size(), z::ZCRCW, crc32_);
    }

    void nextSubpacket(Clock::time_point now) {
        size_t left = data_.size() - pos_;
        size_t n = std::min<size_t>(1024, left);
        // A receiver with a limited buffer gets ZCRCW (wait for ZACK) when it's full.
        bool windowFull = rxBuffer_ && pos_ + n - windowStart_ >= rxBuffer_;
        if (windowFull) n = rxBuffer_ - (pos_ - windowStart_);
        uint8_t end = pos_ + n >= data_.size() ? z::ZCRCE : windowFull ? z::ZCRCW : z::ZCRCG;
        writer_.data(pending_, data_.data() + pos_, n, end, crc32_);
        pos_ += n;
        if (end == z::ZCRCE) {
            header(z::ZEOF, static_cast<uint32_t>(data_.size()));
            enter(S::WaitEof, now);
        } else if (end == z::ZCRCW) {
            enter(S::WaitAck, now);
        }
    }

    void nextFile(Clock::time_point now, bool skipped = false) {
        completed_ += skipped ? 0 : data_.size();
        if (skipped) total_ -= std::min<uint64_t>(total_, data_.size());
        ++index_;
        pos_ = windowStart_ = 0; // now counted in completed_
        pending_.clear();
        startFile(now);
    }

    void sendFin(Clock::time_point now) {
        writer_.hexHeader(pending_, ZHeader::at(z::ZFIN, 0));
        enter(S::WaitFin, now);
    }

    void timeout(Clock::time_point now) {
        if (++tries_ > (s_ == S::WaitFin ? 3 : 6)) {
            if (s_ == S::WaitFin) return finish(); // all files went; the goodbye didn't
            abortLine();
            return fail(s_ == S::WaitInit ? "No ZMODEM receiver answered" : "The computer stopped answering");
        }
        deadline_ = now + seconds(10);
        switch (s_) {
        case S::WaitInit: writer_.hexHeader(pending_, ZHeader::at(z::ZRQINIT, 0)); break;
        case S::WaitFilePos: sendFileHeader(); break;
        case S::WaitAck: // the ZACK went missing: go back to where it was asked
            pos_ = windowStart_;
            header(z::ZDATA, pos_);
            s_ = S::Streaming;
            break;
        case S::WaitEof: header(z::ZEOF, static_cast<uint32_t>(data_.size())); break;
        case S::WaitFin: writer_.hexHeader(pending_, ZHeader::at(z::ZFIN, 0)); break;
        default: break;
        }
    }

    std::vector<std::string> paths_;
    size_t index_ = 0;
    std::vector<uint8_t> data_;
    std::string name_;
    size_t pos_ = 0, windowStart_ = 0;
    uint64_t completed_ = 0;
    uint32_t rxBuffer_ = 0, lastErrorPos_ = 0;
    bool crc32_ = false;
    S s_ = S::Init;
    int tries_ = 0, errors_ = 0;
    Clock::time_point deadline_{};
};

class ZmodemReceiver : public ZmodemBase {
public:
    ZmodemReceiver() { title_ = "Downloading with ZMODEM"; }
    ~ZmodemReceiver() override { closeFile(true); }

    void received(const uint8_t* data, size_t size, Clock::time_point now) override {
        for (size_t i = 0; i < size && !finished(); ++i) {
            if (s_ == S::WaitOO) {
                if (data[i] == 'O' && ++os_ == 2) finish();
                continue;
            }
            parser_.feed(data[i], [&](const ZParser::Event& e) { event(e, now); });
        }
    }

    void poll(Clock::time_point now, std::vector<uint8_t>& out, size_t room) override {
        if (!finished()) {
            if (s_ == S::Start) {
                sendInit();
                giveUpAt_ = now + seconds(60);
                s_ = S::WaitFile;
                deadline_ = now + seconds(5);
            } else if (s_ == S::WaitFile && now > giveUpAt_) {
                abortLine();
                fail(saved_.empty() ? "No ZMODEM sender started" : "The computer stopped sending");
            } else if (s_ == S::WaitFile && now > deadline_) {
                sendInit();
                deadline_ = now + seconds(5);
            } else if (s_ == S::Receiving && now > deadline_) {
                if (++errors_ > 10) {
                    abortLine();
                    fail("The computer stopped sending");
                } else {
                    askFrom(offset_, now);
                }
            } else if (s_ == S::WaitOO && now > deadline_) {
                finish(); // the "OO" is a courtesy
            }
        }
        flush(out, room);
        if (finished() && state_ != State::Done) closeFile(true);
    }

    void setSavePath(const std::string& path) override {
        if (s_ != S::WaitPath) return;
        state_ = State::Running;
        if (path.empty()) {
            writer_.hexHeader(pending_, ZHeader::at(z::ZSKIP, 0));
            s_ = S::WaitFile;
            return;
        }
        file_ = std::fopen(path.c_str(), "wb");
        if (!file_) {
            writer_.hexHeader(pending_, ZHeader::at(z::ZSKIP, 0));
            s_ = S::WaitFile;
            skippedNote_ = "couldn't write " + path;
            return;
        }
        path_ = path;
        offset_ = 0;
        title_ = "Downloading " + baseName(path) + " with ZMODEM";
        s_ = S::Receiving;
        askFrom(0, Clock::now());
    }

    void cancel(std::vector<uint8_t>& out) override {
        FileTransfer::cancel(out);
        closeFile(true);
    }

private:
    enum class S { Start, WaitFile, WaitPath, Receiving, WaitOO };

    void sendInit() {
        ZHeader h;
        h.type = z::ZRINIT;
        h.p[3] = z::CANFDX | z::CANOVIO | z::CANFC32;
        writer_.hexHeader(pending_, h);
    }

    void askFrom(uint32_t pos, Clock::time_point now) {
        writer_.hexHeader(pending_, ZHeader::at(z::ZRPOS, pos));
        deadline_ = now + seconds(10);
    }

    void event(const ZParser::Event& e, Clock::time_point now) {
        if (e.kind == ZParser::Event::Cancel) {
            closeFile(true);
            return fail("Cancelled by the computer");
        }
        if (e.kind == ZParser::Event::Data) return data(e, now);
        const ZHeader& h = e.header;
        switch (h.type) {
        case z::ZRQINIT:
            if (s_ == S::WaitFile) sendInit();
            break;
        case z::ZSINIT:
            expect_ = Expect::Sinit;
            parser_.expectData(e.header32);
            break;
        case z::ZFILE:
            if (s_ == S::WaitFile) {
                expect_ = Expect::FileInfo;
                parser_.expectData(e.header32);
                giveUpAt_ = now + seconds(60);
            }
            break;
        case z::ZDATA:
            if (s_ != S::Receiving) break;
            deadline_ = now + seconds(10);
            if (h.pos() == offset_) {
                expect_ = Expect::FileData;
                parser_.expectData(e.header32);
            } else {
                askFrom(offset_, now);
            }
            break;
        case z::ZEOF:
            if (s_ == S::Receiving && h.pos() == offset_) {
                closeFile(false);
                saved_.push_back(path_);
                completed_ += offset_;
                total_ = 0;
                s_ = S::WaitFile;
                giveUpAt_ = now + seconds(60);
                sendInit();
            }
            break;
        case z::ZFIN:
            writer_.hexHeader(pending_, ZHeader::at(z::ZFIN, 0));
            s_ = S::WaitOO;
            deadline_ = now + seconds(1);
            if (saved_.empty() && !skippedNote_.empty()) message_ = skippedNote_;
            break;
        default: break;
        }
    }

    void data(const ZParser::Event& e, Clock::time_point now) {
        Expect what = expect_;
        expect_ = Expect::None;
        if (what == Expect::Sinit) {
            writer_.hexHeader(pending_, ZHeader::at(z::ZACK, 0));
        } else if (what == Expect::FileInfo) {
            if (!e.good) {
                writer_.hexHeader(pending_, ZHeader::at(z::ZNAK, 0));
                return;
            }
            // "name\0size mtime mode ..."
            std::string name(reinterpret_cast<const char*>(e.data.data()),
                             strnlen(reinterpret_cast<const char*>(e.data.data()), e.data.size()));
            size_t nul = name.size();
            unsigned long long size = 0;
            if (nul + 1 < e.data.size())
                size = std::strtoull(reinterpret_cast<const char*>(e.data.data()) + nul + 1, nullptr, 10);
            std::string clean;
            for (char c : baseName(name)) clean += (static_cast<unsigned char>(c) < 0x20 || c == '/' || c == '\\') ? '_' : c;
            offeredName_ = clean.empty() ? "download" : clean;
            done_ = completed_;
            total_ = completed_ + size;
            title_ = "Downloading " + offeredName_ + " with ZMODEM";
            s_ = S::WaitPath;
            state_ = State::NeedsSavePath;
        } else if (what == Expect::FileData && s_ == S::Receiving) {
            if (!e.good) {
                ++errors_;
                return askFrom(offset_, now);
            }
            if (std::fwrite(e.data.data(), 1, e.data.size(), file_) != e.data.size()) {
                writer_.hexHeader(pending_, ZHeader::at(z::ZFERR, 0));
                closeFile(true);
                return fail("Cannot write " + path_);
            }
            offset_ += static_cast<uint32_t>(e.data.size());
            done_ = completed_ + offset_;
            deadline_ = now + seconds(10);
            errors_ = 0;
            if (e.end == z::ZCRCG || e.end == z::ZCRCQ) expect_ = Expect::FileData; // more follow
            if (e.end == z::ZCRCQ || e.end == z::ZCRCW) writer_.hexHeader(pending_, ZHeader::at(z::ZACK, offset_));
        }
    }

    void closeFile(bool remove) {
        if (!file_) return;
        std::fclose(file_);
        file_ = nullptr;
        if (remove) std::remove(path_.c_str()); // don't leave half a file
    }

    enum class Expect { None, Sinit, FileInfo, FileData };
    S s_ = S::Start;
    Expect expect_ = Expect::None;
    std::FILE* file_ = nullptr;
    std::string path_, skippedNote_;
    uint32_t offset_ = 0;
    uint64_t completed_ = 0;
    int errors_ = 0, os_ = 0;
    Clock::time_point deadline_{}, giveUpAt_{};
};

} // namespace

// --- Factories --------------------------------------------------------------------

std::unique_ptr<FileTransfer> sendTextFile(const std::string& path, const TextSendOptions& options,
                                           std::string* error) {
    std::vector<uint8_t> text;
    if (!readFile(path, text, error)) return nullptr;
    return std::make_unique<TextSender>(text, baseName(path), options);
}

std::unique_ptr<FileTransfer> xmodemSend(const std::string& path, bool oneK, std::string* error) {
    std::vector<uint8_t> data;
    if (!readFile(path, data, error)) return nullptr;
    return std::make_unique<XmodemSender>(std::move(data), cpmFileName(path), oneK);
}

std::unique_ptr<FileTransfer> xmodemReceive(const std::string& path, std::string* error) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        if (error) *error = "cannot write " + path + ": " + std::strerror(errno);
        return nullptr;
    }
    return std::make_unique<XmodemReceiver>(f, path);
}

std::unique_ptr<FileTransfer> zmodemSend(const std::vector<std::string>& paths, std::string* error) {
    for (const auto& p : paths) {
        std::FILE* f = std::fopen(p.c_str(), "rb");
        if (!f) {
            if (error) *error = "cannot open " + p + ": " + std::strerror(errno);
            return nullptr;
        }
        std::fclose(f);
    }
    if (paths.empty()) {
        if (error) *error = "no files to send";
        return nullptr;
    }
    return std::make_unique<ZmodemSender>(paths);
}

std::unique_ptr<FileTransfer> zmodemReceive() {
    return std::make_unique<ZmodemReceiver>();
}

} // namespace h89
