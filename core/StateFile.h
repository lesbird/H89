#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <string>
#include <vector>

namespace h89 {

// Save-state serialization: little-endian values grouped in tagged,
// length-prefixed sections, so a reader can tell which component a section
// belongs to and refuse files that don't match.
class StateWriter {
public:
    void u8(uint8_t v) { out_.push_back(v); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void u16(uint16_t v) { u8(v & 0xFF), u8(v >> 8); }
    void u32(uint32_t v) { u16(v & 0xFFFF), u16(v >> 16); }
    void u64(uint64_t v) { u32(v & 0xFFFFFFFF), u32(v >> 32); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void bytes(const uint8_t* p, size_t n) {
        u32(static_cast<uint32_t>(n));
        out_.insert(out_.end(), p, p + n);
    }
    void bytes(const std::vector<uint8_t>& v) { bytes(v.data(), v.size()); }
    template <size_t N>
    void bytes(const std::array<uint8_t, N>& a) { bytes(a.data(), N); }
    void bytes(const std::deque<uint8_t>& d) { bytes(std::vector<uint8_t>(d.begin(), d.end())); }
    void str(const std::string& s) { bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

    // Starts a section; its length is filled in by end().
    void begin(const char tag[5]) {
        out_.insert(out_.end(), tag, tag + 4);
        starts_.push_back(out_.size());
        u32(0);
    }
    void end() {
        size_t at = starts_.back();
        starts_.pop_back();
        uint32_t len = static_cast<uint32_t>(out_.size() - at - 4);
        for (int i = 0; i < 4; ++i) out_[at + i] = static_cast<uint8_t>(len >> (8 * i));
    }

    const std::vector<uint8_t>& data() const { return out_; }

private:
    std::vector<uint8_t> out_;
    std::vector<size_t> starts_;
};

class StateReader {
public:
    explicit StateReader(const std::vector<uint8_t>& data) : in_(data) {}

    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }
    void fail(const std::string& why) {
        if (ok_) error_ = why;
        ok_ = false;
    }

    uint8_t u8() {
        if (pos_ >= limit()) {
            fail("the state file is truncated");
            return 0;
        }
        return in_[pos_++];
    }
    bool boolean() { return u8() != 0; }
    uint16_t u16() { uint16_t lo = u8(); return static_cast<uint16_t>(lo | (u8() << 8)); }
    uint32_t u32() { uint32_t lo = u16(); return lo | (static_cast<uint32_t>(u16()) << 16); }
    uint64_t u64() { uint64_t lo = u32(); return lo | (static_cast<uint64_t>(u32()) << 32); }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    std::vector<uint8_t> bytes() {
        uint32_t n = u32();
        if (!ok_ || n > limit() - pos_) {
            fail("the state file is truncated");
            return {};
        }
        std::vector<uint8_t> v(in_.begin() + static_cast<std::ptrdiff_t>(pos_),
                               in_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        return v;
    }
    template <size_t N>
    void bytes(std::array<uint8_t, N>& a) {
        std::vector<uint8_t> v = bytes();
        if (v.size() != N) return fail("a memory block has the wrong size");
        std::memcpy(a.data(), v.data(), N);
    }
    void bytes(std::deque<uint8_t>& d) {
        std::vector<uint8_t> v = bytes();
        d.assign(v.begin(), v.end());
    }
    std::string str() {
        std::vector<uint8_t> v = bytes();
        return std::string(v.begin(), v.end());
    }

    // Enters a section, which must carry `tag`; end() checks it was read
    // exactly and moves past it.
    bool begin(const char tag[5]) {
        if (!ok_) return false;
        if (limit() - pos_ < 8 || std::memcmp(&in_[pos_], tag, 4) != 0) {
            fail(std::string("missing section ") + tag);
            return false;
        }
        pos_ += 4;
        uint32_t len = u32();
        if (len > limit() - pos_) {
            fail(std::string("section ") + tag + " is truncated");
            return false;
        }
        ends_.push_back(pos_ + len);
        return true;
    }
    void end() {
        if (ends_.empty()) return;
        if (ok_ && pos_ != ends_.back()) fail("a section has the wrong length (different emulator version?)");
        pos_ = ends_.back();
        ends_.pop_back();
    }

private:
    size_t limit() const { return ends_.empty() ? in_.size() : ends_.back(); }

    const std::vector<uint8_t>& in_;
    size_t pos_ = 0;
    std::vector<size_t> ends_;
    bool ok_ = true;
    std::string error_;
};

} // namespace h89
