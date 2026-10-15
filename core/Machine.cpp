#include "Machine.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

namespace h89 {

namespace {

// RST n opcodes placed on the bus during interrupt acknowledge.
constexpr uint8_t rst(int n) { return 0xC7 | (n << 3); }

constexpr int kConsoleRst = 3;
constexpr int kTimerRst = 1;

} // namespace

std::vector<uint8_t> loadFile(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return {};
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

Machine::Machine(MachineConfig config) : config_(std::move(config)), cpu_(*this) {
    int kb = std::clamp(config_.ramKB, 16, 64);
    ramTop_ = kb == 64 ? 0x10000 : 0x2000 + kb * 1024;
    if (!config_.h17Rom.empty()) h17_ = std::make_unique<H17>();
    if (config_.hasTerminal()) {
        terminal_ = std::make_unique<H19>(config_.terminal);
        terminal_->serialOut = [this](uint8_t b) { console_.receive(b); };
    }
    console_.onTransmit = [this](uint8_t b) {
        if (terminal_) terminal_->serialIn(b);
        if (consoleOutput) consoleOutput(b);
    };
    // The terminal holds CTS, DSR and DCD active.
    console_.setModemInputs(0xB0);
    reset();
}

void Machine::reset() {
    ram_.fill(0);
    h17Ram_.fill(0);
    cpu_.reset();
    console_.reset();
    console_.setModemInputs(0xB0);
    gpp_ = 0;
    timerCountdown_ = kTimerCycles;
    timerPending_ = false;
    frames_ = 0;
    if (terminal_) terminal_->reset();
    if (h17_) h17_->reset();
    terminalLag_ = 0;
    updateIrq();
}

uint8_t Machine::memRead(uint16_t addr) {
    if (addr >= 0x2000) return addr < ramTop_ ? ram_[addr] : 0xFF;
    if ((gpp_ & GPP_ORG0) && ramTop_ == 0x10000) return ram_[addr];
    if (addr < 0x1000) return addr < config_.monitorRom.size() ? config_.monitorRom[addr] : 0xFF;
    if (config_.h17Rom.empty() || addr < 0x1400) return 0xFF;
    if (addr < 0x1800) return h17Ram_[addr - 0x1400];
    uint16_t off = addr - 0x1800;
    return off < config_.h17Rom.size() ? config_.h17Rom[off] : 0xFF;
}

void Machine::memWrite(uint16_t addr, uint8_t value) {
    if (addr >= 0x2000) {
        if (addr < ramTop_) ram_[addr] = value;
    } else if ((gpp_ & GPP_ORG0) && ramTop_ == 0x10000) {
        ram_[addr] = value;
    } else if (addr >= 0x1400 && addr < 0x1800 && !config_.h17Rom.empty()) {
        h17Ram_[addr - 0x1400] = value;
    }
}

uint8_t Machine::ioRead(uint16_t port) {
    uint8_t v = readPort(port & 0xFF);
    if (ioTrace) ioTrace(false, port & 0xFF, v);
    return v;
}

uint8_t Machine::readPort(uint8_t p) {
    if (h17_ && p >= 0x7C && p <= 0x7F) return h17_->read(p & 3);
    switch (p) {
    case 0xE8: case 0xE9: case 0xEA: case 0xEB:
    case 0xEC: case 0xED: case 0xEE: case 0xEF:
        return console_.read(p & 7);
    case 0xF2:
        return config_.sw501;
    case 0xF0: case 0xF1: case 0xFA: case 0xFB:
        // H8 front-panel ports don't exist on the H89; touching them raises
        // an NMI and the monitor ROM emulates the access in software.
        cpu_.triggerNmi();
        return 0xFF;
    default:
        return 0xFF;
    }
}

void Machine::ioWrite(uint16_t port, uint8_t value) {
    uint8_t p = port & 0xFF;
    if (ioTrace) ioTrace(true, p, value);
    if (h17_ && p >= 0x7C && p <= 0x7F) {
        h17_->write(p & 3, value);
        return;
    }
    switch (p) {
    case 0xE8: case 0xE9: case 0xEA: case 0xEB:
    case 0xEC: case 0xED: case 0xEE: case 0xEF:
        console_.write(p & 7, value);
        break;
    case 0xF2:
        writeGpp(value);
        break;
    case 0xF0: case 0xF1: case 0xFA: case 0xFB:
        cpu_.triggerNmi();
        break;
    default:
        break;
    }
    updateIrq();
}

void Machine::writeGpp(uint8_t value) {
    gpp_ = value;
    if (!(gpp_ & GPP_TIMER_ENABLE)) timerPending_ = false;
    // TODO: single-step interrupt (GPP bit 0, RST 2) for the monitor's
    // single-step command.
}

// Interrupt levels go through a priority encoder; the highest level wins.
uint8_t Machine::intAck() {
    uint8_t op;
    if (console_.irq()) {
        op = rst(kConsoleRst);
    } else if (timerPending_) {
        timerPending_ = false; // acknowledge clears the clock flip-flop
        op = rst(kTimerRst);
    } else {
        op = rst(7); // spurious
    }
    updateIrq();
    return op;
}

void Machine::updateIrq() {
    cpu_.setIrq(timerPending_ || console_.irq());
}

int Machine::run(int cycles) {
    int done = 0;
    while (done < cycles) {
        int t = cpu_.step();
        done += t;
        console_.tick(t);
        if (h17_) h17_->tick(t);
        if (gpp_ & GPP_TIMER_ENABLE) {
            timerCountdown_ -= t;
            if (timerCountdown_ <= 0) {
                timerCountdown_ += kTimerCycles;
                timerPending_ = true;
            }
        }
        updateIrq();
        // Both Z80s run at 2.048 MHz; keep the terminal in step.
        if (terminal_) {
            terminalLag_ += t;
            while (terminalLag_ > 0) terminalLag_ -= terminal_->step();
        }
    }
    return done;
}

void Machine::runFrame() {
    run(kCpuHz / kFrameHz);
    ++frames_;
    if (!terminal_) drawTestPattern();
}

// Placeholder until the terminal board exists: a border, a character-cell
// grid, and a block cursor that blinks like the H19's.
void Machine::drawTestPattern() {
    auto& px = fb_.pixels;
    std::fill(px.begin(), px.end(), 0);

    for (int y = 0; y < kScreenHeight; ++y) {
        for (int x = 0; x < kScreenWidth; ++x) {
            bool border = x < 2 || x >= kScreenWidth - 2 || y < 1 || y >= kScreenHeight - 1;
            bool grid = (x % 8 == 0) && (y % 10 == 0);
            if (border) px[y * kScreenWidth + x] = 255;
            else if (grid) px[y * kScreenWidth + x] = 96;
        }
    }

    // Blinking cursor in the top-left cell, ~1.9 Hz like the 6845 fast blink.
    if ((frames_ / 16) % 2 == 0) {
        for (int y = 2; y < 10; ++y)
            for (int x = 8; x < 16; ++x) px[y * kScreenWidth + x] = 255;
    }
}

} // namespace h89
