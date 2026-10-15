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
constexpr int kH37Rst = 4;
constexpr uint8_t kOpEI = 0xFB;

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
    if (config_.h37) h37_ = std::make_unique<H37>();
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
    if (h37_) h37_->reset();
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
    if (h37_ && p >= 0x78 && p <= 0x7B) return h37_->read(p & 3);
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
    if (h37_ && p >= 0x78 && p <= 0x7B) {
        h37_->write(p & 3, value);
        updateIrq();
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
// The Z-89-37 sits in front: its DRQ is answered with EI (just ending a
// HALT), its INTRQ with RST 4, and while its DRQ interrupts are enabled the
// other levels are blocked.
uint8_t Machine::intAck() {
    uint8_t op;
    if (h37_ && h37_->drqInterrupt()) {
        op = kOpEI;
    } else if (h37_ && h37_->irq()) {
        op = rst(kH37Rst);
    } else if (h37_ && h37_->blocksInterrupts()) {
        op = 0x00; // NOP: nothing is asking
    } else if (console_.irq()) {
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
    bool others = timerPending_ || console_.irq();
    if (h37_) {
        if (h37_->blocksInterrupts()) others = false;
        others = others || h37_->irq() || h37_->drqInterrupt();
    }
    cpu_.setIrq(others);
}

int Machine::run(int cycles) {
    int done = 0;
    while (done < cycles) {
        int t = cpu_.step();
        done += t;
        console_.tick(t);
        if (h17_) h17_->tick(t);
        if (h37_) h37_->tick(t);
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

namespace {

constexpr char kStateMagic[] = "H89STATE";
constexpr uint32_t kStateVersion = 1;

} // namespace

uint64_t Machine::romHash() const {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const auto* rom : {&config_.monitorRom, &config_.h17Rom, &config_.terminal.rom, &config_.terminal.font}) {
        for (uint8_t b : *rom) h = (h ^ b) * 0x100000001b3ull;
        h = (h ^ 0xFF) * 0x100000001b3ull; // separator
    }
    return h;
}

std::vector<uint8_t> Machine::saveState() {
    StateWriter w;
    for (const char* c = kStateMagic; *c; ++c) w.u8(static_cast<uint8_t>(*c));
    w.u32(kStateVersion);
    w.begin("CONF");
    w.u64(romHash());
    w.u8(config_.sw501);
    w.i32(config_.ramKB);
    w.boolean(h17_ != nullptr), w.boolean(h37_ != nullptr), w.boolean(terminal_ != nullptr);
    w.end();
    saveDisks(w);
    w.begin("H89 ");
    cpu_.saveState(w);
    console_.saveState(w);
    w.bytes(ram_);
    w.bytes(h17Ram_);
    w.u8(gpp_), w.i32(timerCountdown_), w.boolean(timerPending_), w.i32(terminalLag_), w.u64(frames_);
    w.end();
    if (terminal_) terminal_->saveState(w);
    if (h17_) h17_->saveState(w);
    if (h37_) h37_->saveState(w);
    return w.data();
}

void Machine::saveDisks(StateWriter& w) {
    w.begin("DSKS");
    auto disk = [&](bool loaded, const std::string& path, bool readOnly, uint64_t hash) {
        w.boolean(loaded);
        if (loaded) w.str(path), w.boolean(readOnly), w.u64(hash);
    };
    if (h17_)
        for (int d = 0; d < H17::kDrives; ++d) {
            const H17Disk& k = h17_->disk(d);
            disk(k.loaded(), k.path(), k.readOnly(), k.loaded() ? k.contentHash() : 0);
        }
    if (h37_)
        for (int d = 0; d < H37::kDrives; ++d) {
            const SoftDisk& k = h37_->disk(d);
            disk(k.loaded(), k.path(), k.readOnly(), k.loaded() ? k.contentHash() : 0);
        }
    w.end();
}

// Remounts the snapshot's disks, keeping ones already mounted from the same
// file so the emulated OS sees the same in-memory image.
void Machine::loadDisks(StateReader& r, std::vector<std::string>* warnings) {
    if (!r.begin("DSKS")) return;
    auto warn = [&](const std::string& message) {
        if (warnings) warnings->push_back(message);
    };
    auto restore = [&](auto& controller, int drives, const char* prefix) {
        for (int d = 0; d < drives && r.ok(); ++d) {
            std::string name = prefix + std::to_string(d) + ":";
            if (!r.boolean()) {
                controller.ejectDisk(d);
                continue;
            }
            std::string path = r.str();
            bool readOnly = r.boolean();
            uint64_t hash = r.u64();
            auto& disk = controller.disk(d);
            if (!(disk.loaded() && disk.path() == path && disk.readOnly() == readOnly)) {
                std::string err;
                if (!controller.insertDisk(d, path, readOnly, &err)) {
                    controller.ejectDisk(d);
                    warn(name + " " + path + " couldn't be mounted: " + err);
                    continue;
                }
            }
            if (disk.contentHash() != hash)
                warn(name + " " + path + " has changed since this state was saved; the restored system may "
                            "not match what is on the disk.");
        }
    };
    if (h17_) restore(*h17_, H17::kDrives, "SY");
    if (h37_) restore(*h37_, H37::kDrives, "DK");
    r.end();
}

bool Machine::loadState(const std::vector<uint8_t>& data, std::string* error, std::vector<std::string>* warnings) {
    std::vector<uint8_t> backup = saveState();
    if (loadStateUnchecked(data, error, warnings)) return true;
    loadStateUnchecked(backup, nullptr, nullptr);
    return false;
}

bool Machine::loadStateUnchecked(const std::vector<uint8_t>& data, std::string* error,
                                 std::vector<std::string>* warnings) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    StateReader r(data);
    for (const char* c = kStateMagic; *c; ++c)
        if (r.u8() != static_cast<uint8_t>(*c)) return fail("This isn't an H89 saved state.");
    if (r.u32() != kStateVersion) return fail("This state was saved by a different version of the emulator.");
    if (!r.begin("CONF")) return fail(r.error());
    if (r.u64() != romHash()) return fail("This state was saved with different ROMs.");
    uint8_t sw501 = r.u8();
    int ramKB = r.i32();
    bool hasH17 = r.boolean(), hasH37 = r.boolean(), hasTerminal = r.boolean();
    r.end();
    if (ramKB != config_.ramKB || hasH17 != (h17_ != nullptr) || hasH37 != (h37_ != nullptr) ||
        hasTerminal != (terminal_ != nullptr))
        return fail("This state was saved from a differently configured machine.");
    config_.sw501 = sw501;

    loadDisks(r, warnings);
    if (r.begin("H89 ")) {
        cpu_.loadState(r);
        console_.loadState(r);
        r.bytes(ram_);
        r.bytes(h17Ram_);
        gpp_ = r.u8(), timerCountdown_ = r.i32(), timerPending_ = r.boolean(), terminalLag_ = r.i32();
        frames_ = r.u64();
        r.end();
    }
    if (terminal_) terminal_->loadState(r);
    if (h17_) h17_->loadState(r);
    if (h37_) h37_->loadState(r);
    if (!r.ok()) return fail("The state file is damaged: " + r.error() + ".");
    updateIrq();
    return true;
}

} // namespace h89
