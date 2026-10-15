#pragma once

#include "FileTransfer.h"
#include "SerialPort.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace h89 {

class H19;

// Runs an H19 on its own as a terminal for a real computer, through a host
// serial port (a USB serial adapter to an H8, say).
//
// The line runs at the port's rate, not SW401's. Received bytes wait here
// and are handed to the H19's UART only once its firmware has read the
// previous one, and not while the firmware has sent XOFF to say its buffer is
// full; that XOFF and the XON after it are kept from the computer, since the
// waiting happens here. So nothing overruns at any rate, and while bytes are
// waiting the H19 runs faster than real time to catch up, as fast as the
// host allows. Ctrl-S and Ctrl-Q typed on the keyboard do go out. If the port goes
// away (an adapter unplugged), it is reopened once it comes back.
class TerminalLink {
public:
    explicit TerminalLink(H19& terminal);
    ~TerminalLink();
    TerminalLink(const TerminalLink&) = delete;
    TerminalLink& operator=(const TerminalLink&) = delete;

    // Opens the port (closing any previous one). On failure the terminal
    // still runs, offline, and the port is retried every second.
    bool open(const SerialSettings& settings, std::string* error = nullptr);
    void close();
    bool connected() const { return port_.isOpen(); }
    const SerialSettings& settings() const { return settings_; }
    // "/dev/cu.usbserial-1410 at 115200 8N1", or why it isn't connected.
    std::string description() const;

    // Advances the terminal by one 1/60 s frame (more while catching up)
    // and moves data to and from the port.
    void runFrame();
    // Runs the terminal for `cycles` CPU cycles, moving data as it goes.
    void run(int cycles);

    // Bytes received from the port so far (for scripted tests).
    const std::string& received() const { return received_; }

    // A file transfer over the line (see FileTransfer.h). While one runs, the
    // protocols' bytes stay off the screen and the keyboard is ignored; text
    // being sent still shows as the computer echoes it. It is paced at the
    // port's rate. False if one is already running.
    bool startTransfer(std::unique_ptr<FileTransfer> transfer);
    // The current or last transfer (null if none), until clearTransfer().
    FileTransfer* transfer() const { return transfer_.get(); }
    bool transferRunning() const { return transfer_ && !transfer_->finished(); }
    void cancelTransfer();
    void clearTransfer();

private:
    void service();

    H19& terminal_;
    SerialPort port_;
    SerialSettings settings_;
    bool wanted_ = false; // open() was called and not closed
    std::string error_;
    std::chrono::steady_clock::time_point retryAt_{};
    std::deque<uint8_t> inbound_;
    std::vector<uint8_t> outbound_;
    bool breakOn_ = false;
    bool held_ = false; // the firmware sent XOFF: its input buffer is full
    std::string received_;
    std::unique_ptr<FileTransfer> transfer_;
    double sendBudget_ = 0; // bytes the line can take now
    std::chrono::steady_clock::time_point lastBudget_{};
};

} // namespace h89
