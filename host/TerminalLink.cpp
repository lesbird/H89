#include "TerminalLink.h"

#include "terminal/H19.h"

#include <algorithm>

namespace h89 {

namespace {

constexpr int kFrameCycles = H19::kCpuHz / 60;
// How long a frame may spend catching up on received data.
constexpr auto kCatchUpBudget = std::chrono::milliseconds(10);
// Received text kept for received(); older text is dropped.
constexpr size_t kReceivedLimit = 1 << 20;

} // namespace

TerminalLink::TerminalLink(H19& terminal) : terminal_(terminal) {
    terminal_.serialOut = [this](uint8_t b) {
        // Keys would corrupt a protocol transfer.
        if (transferRunning() && !transfer_->echoesToScreen()) return;
        uint8_t c = b & 0x7F;
        if ((c == 0x13 || c == 0x11) && !terminal_.takeTypedFlowKey(c)) {
            held_ = c == 0x13; // the firmware's own flow control
            return;
        }
        if (settings_.dataBits == 7) b = c;
        outbound_.push_back(b);
    };
}

TerminalLink::~TerminalLink() {
    terminal_.serialOut = nullptr;
    terminal_.setLineRate(0);
}

bool TerminalLink::open(const SerialSettings& settings, std::string* error) {
    close();
    settings_ = settings;
    wanted_ = true;
    terminal_.setLineRate(settings.baud);
    // The computer's end is always there as far as the H19 can tell.
    terminal_.setModemInputs(0xB0);
    if (port_.open(settings, &error_)) {
        error_.clear();
        return true;
    }
    if (error) *error = error_;
    retryAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    return false;
}

void TerminalLink::close() {
    if (breakOn_) port_.setBreak(false);
    port_.close();
    wanted_ = false;
    breakOn_ = false;
    held_ = false;
    error_.clear();
    outbound_.clear();
}

std::string TerminalLink::description() const {
    if (!wanted_) return "not connected";
    std::string where = settings_.device + " at " + std::to_string(settings_.baud) + " " + settings_.format() +
                        (settings_.rtsCts ? " RTS/CTS" : "");
    if (port_.isOpen()) return where;
    // The port's errors start with its name, already given.
    std::string why = error_.rfind(settings_.device + ": ", 0) == 0 ? error_.substr(settings_.device.size() + 2) : error_;
    return where + ": not connected" + (why.empty() ? "" : " (" + why + ")");
}

void TerminalLink::service() {
    if (!wanted_) {
        outbound_.clear();
        return;
    }
    if (!port_.isOpen()) {
        auto now = std::chrono::steady_clock::now();
        if (now < retryAt_) return;
        retryAt_ = now + std::chrono::seconds(1);
        std::string err;
        if (!port_.open(settings_, &err)) {
            error_ = err;
            return;
        }
        error_.clear();
        breakOn_ = false;
    }
    uint8_t buf[4096];
    for (;;) {
        int n = port_.read(buf, sizeof buf);
        if (n <= 0) {
            if (n < 0) error_ = "the port went away";
            break;
        }
        auto now = std::chrono::steady_clock::now();
        bool show = true;
        if (transferRunning()) {
            show = transfer_->echoesToScreen();
            transfer_->received(buf, static_cast<size_t>(n), now);
        }
        if (show) inbound_.insert(inbound_.end(), buf, buf + n);
        received_.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
        if (received_.size() > kReceivedLimit) received_.erase(0, received_.size() - kReceivedLimit / 2);
    }
    if (transfer_ && port_.isOpen()) { // a finished one may still owe a reply
        // Hand the transfer as much as the line can carry: a byte is 10 bits.
        auto now = std::chrono::steady_clock::now();
        double rate = settings_.baud / 10.0;
        if (lastBudget_ != std::chrono::steady_clock::time_point{})
            sendBudget_ += rate * std::chrono::duration<double>(now - lastBudget_).count();
        lastBudget_ = now;
        sendBudget_ = std::min(sendBudget_, std::max(64.0, rate / 20)); // at most 50 ms ahead
        if (outbound_.size() < 64 && sendBudget_ >= 1) {
            size_t before = outbound_.size();
            transfer_->poll(now, outbound_, static_cast<size_t>(sendBudget_));
            sendBudget_ -= static_cast<double>(outbound_.size() - before);
        }
    }
    if (!outbound_.empty() && port_.isOpen()) {
        int n = port_.write(outbound_.data(), outbound_.size());
        if (n > 0) outbound_.erase(outbound_.begin(), outbound_.begin() + n);
        else if (n < 0) error_ = "the port went away";
    }
    // BREAK key: the firmware holds the line in the break condition.
    bool breakNow = terminal_.sendingBreak();
    if (breakNow != breakOn_ && port_.isOpen()) {
        port_.setBreak(breakNow);
        breakOn_ = breakNow;
    }
}

bool TerminalLink::startTransfer(std::unique_ptr<FileTransfer> transfer) {
    if (transferRunning() || !transfer) return false;
    transfer_ = std::move(transfer);
    sendBudget_ = 0;
    lastBudget_ = {};
    return true;
}

void TerminalLink::cancelTransfer() {
    if (!transferRunning()) return;
    transfer_->cancel(outbound_);
}

void TerminalLink::clearTransfer() {
    if (!transferRunning()) transfer_.reset();
}

void TerminalLink::run(int cycles) {
    int done = 0;
    while (done < cycles) {
        if (!held_ && !inbound_.empty() && terminal_.serialReady()) {
            terminal_.serialIn(inbound_.front());
            inbound_.pop_front();
        }
        done += terminal_.run(1);
    }
}

void TerminalLink::runFrame() {
    service();
    run(kFrameCycles);
    // Catch up on a backlog: the real H19 couldn't keep up with fast lines,
    // but the emulated one can run many times faster than it.
    auto start = std::chrono::steady_clock::now();
    while (!inbound_.empty() && std::chrono::steady_clock::now() - start < kCatchUpBudget) {
        run(kFrameCycles / 16);
        service();
    }
    service();
}

} // namespace h89
