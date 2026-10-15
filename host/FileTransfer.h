#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace h89 {

// Moving files over the terminal's serial line: sending a text file a
// character at a time, and XMODEM and ZMODEM uploads and downloads.
//
// A transfer is a state machine: received() takes what the computer sends,
// and poll() adds what to send next (at most `room` bytes, so the caller can
// pace the line). Neither blocks; poll() also handles timeouts.
class FileTransfer {
public:
    using Clock = std::chrono::steady_clock;

    enum class State {
        Running,
        NeedsSavePath, // a ZMODEM download was offered a file: call setSavePath()
        Done,
        Failed,
        Cancelled,
    };

    virtual ~FileTransfer();

    virtual void received(const uint8_t* data, size_t size, Clock::time_point now) = 0;
    // Keep calling it after the transfer finishes until it adds nothing:
    // the last reply (an ACK, say) may still be waiting to go.
    virtual void poll(Clock::time_point now, std::vector<uint8_t>& out, size_t room) = 0;
    // Whether what the computer sends should still reach the screen (yes for
    // text, which it echoes; no for the protocols).
    virtual bool echoesToScreen() const { return false; }
    // Stops, adding what tells the other end to stop too.
    virtual void cancel(std::vector<uint8_t>& out);

    // ZMODEM downloads: where to save the file the sender named (""
    // skips it).
    virtual void setSavePath(const std::string& path);
    const std::string& offeredName() const { return offeredName_; }

    State state() const { return state_; }
    bool finished() const { return state_ == State::Done || state_ == State::Failed || state_ == State::Cancelled; }
    // "Uploading HELLO.ASM with XMODEM"
    const std::string& title() const { return title_; }
    // "12,345 of 40,960 bytes", or why it failed.
    std::string status() const;
    uint64_t bytesDone() const { return done_; }
    uint64_t bytesTotal() const { return total_; } // 0 if unknown
    // Files written by a download.
    const std::vector<std::string>& savedFiles() const { return saved_; }

protected:
    void fail(const std::string& why) {
        if (!finished()) state_ = State::Failed, message_ = why;
    }
    void finish(const std::string& note = {}) {
        if (!finished()) state_ = State::Done, message_ = note;
    }

    State state_ = State::Running;
    std::string title_;
    std::string message_;
    std::string offeredName_;
    uint64_t done_ = 0, total_ = 0;
    std::vector<std::string> saved_;
};

struct TextSendOptions {
    enum class LineEnding { CR, CRLF, LF, AsIs };
    LineEnding lineEnding = LineEnding::CR; // what each line ends with
    int charDelayMs = 0;                    // after every character
    int lineDelayMs = 0;                    // after every line
};

// A text file typed out a character at a time. Line ends become
// `lineEnding`, a CP/M end-of-file (^Z) ends the text, and XOFF (^S) from the
// computer pauses sending until XON (^Q).
std::unique_ptr<FileTransfer> sendTextFile(const std::string& path, const TextSendOptions& options,
                                           std::string* error = nullptr);

// XMODEM with checksums or CRCs, whichever the receiver asks for; `oneK`
// sends 1024-byte blocks when it asks for CRCs (XMODEM-1K).
std::unique_ptr<FileTransfer> xmodemSend(const std::string& path, bool oneK, std::string* error = nullptr);
// Saves to `path`, exactly as received: XMODEM pads the end to a whole
// 128-byte block (CP/M's record size) with ^Z.
std::unique_ptr<FileTransfer> xmodemReceive(const std::string& path, std::string* error = nullptr);

// ZMODEM, one or more files, sent under CP/M-style 8.3 upper-case names.
std::unique_ptr<FileTransfer> zmodemSend(const std::vector<std::string>& paths, std::string* error = nullptr);
// Asks (NeedsSavePath) where to save each file the sender offers.
std::unique_ptr<FileTransfer> zmodemReceive();

// The 8.3 upper-case name a file is sent under: "hello world.asm" ->
// "HELLOWOR.ASM".
std::string cpmFileName(const std::string& hostName);

} // namespace h89
