#pragma once

#include "cpu/Z80.h"
#include "devices/H17.h"
#include "devices/H37.h"
#include "devices/Ins8250.h"
#include "terminal/H19.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace h89 {

// SW501 on the CPU board, read at port F2h.
namespace sw501 {
constexpr uint8_t kPort174Mask = 0x03;  // device at 174Q (7Ch): 00 = H-88-1 (H17)
constexpr uint8_t kPort170Mask = 0x0C;  // device at 170Q (78h)
constexpr uint8_t kBootFrom170 = 0x10;  // primary boot device at 170Q instead of 174Q
constexpr uint8_t kNormal = 0x20;       // MTR-90: 0 runs the boot-time path at 0E1Ah
constexpr uint8_t kBaud19200 = 0x40;    // console at 19200 instead of 9600 baud
constexpr uint8_t kAutoBoot = 0x80;
} // namespace sw501

struct MachineConfig {
    std::vector<uint8_t> monitorRom; // 2K or 4K at 0000h (MTR-88/89/90)
    std::vector<uint8_t> h17Rom;     // 2K at 1800h; empty means no H-88-1 controller
    bool h37 = true;                 // Z-89-37 soft-sectored controller at 78h
    int ramKB = 64;                  // 16, 32, 48 or 64
    uint8_t sw501 = sw501::kNormal;
    // Built-in H19 terminal. Without its ROMs the console serial port is
    // exposed only through Machine::consoleOutput / consoleInput.
    H19Config terminal;
    bool hasTerminal() const { return !terminal.rom.empty() && !terminal.font.empty(); }
};

// Reads a whole file; returns an empty vector (and sets `error`) on failure.
std::vector<uint8_t> loadFile(const std::string& path, std::string* error = nullptr);

// The H89 computer: CPU board (Z80, ROM, RAM, GPP, 2 ms clock, console 8250)
// wired to the built-in H19 terminal board, plus the H-88-1 (H17) floppy
// controller when its ROM is present and the Z-89-37 (H37) controller.
class Machine : private Z80Bus {
public:
    static constexpr int kCpuHz = 2'048'000;
    static constexpr int kFrameHz = 60;
    static constexpr int kTimerCycles = kCpuHz / 500; // 2 ms

    // General purpose port bits (OUT F2h)
    enum : uint8_t {
        GPP_SINGLE_STEP = 0x01,
        GPP_TIMER_ENABLE = 0x02,
        GPP_ORG0 = 0x20, // map RAM over the ROM area (CP/M)
    };

    Machine() : Machine(MachineConfig{}) {}
    explicit Machine(MachineConfig config);

    void reset();
    // Runs at least `cycles` CPU cycles; returns the number actually run.
    int run(int cycles);
    // Advances emulation by one 1/60 s video frame.
    void runFrame();

    // Bytes the CPU sends to the terminal (observed even when the built-in
    // terminal is present).
    std::function<void(uint8_t)> consoleOutput;
    // A byte arriving from the terminal side, at the console baud rate. Use
    // this only without a built-in terminal; otherwise type on terminal().
    void consoleInput(uint8_t byte) { console_.receive(byte); }
    bool consoleInputIdle() const { return console_.receiveIdle(); }

    // Optional hook called on every I/O port access (for debugging).
    std::function<void(bool write, uint8_t port, uint8_t value)> ioTrace;

    const MachineConfig& config() const { return config_; }
    // SW501 is read live by the monitor, so changes apply at the next boot.
    void setSw501(uint8_t value) { config_.sw501 = value; }
    // The terminal, or null when MachineConfig has no terminal ROMs.
    H19* terminal() { return terminal_.get(); }
    // The H17 floppy controller, or null without the H17 ROM.
    H17* h17() { return h17_.get(); }
    // The H37 floppy controller, or null if not configured.
    H37* h37() { return h37_.get(); }
    // The terminal's screen, or a test pattern without a terminal.
    const Framebuffer& framebuffer() const { return terminal_ ? terminal_->framebuffer() : fb_; }
    uint64_t frameCount() const { return frames_; }
    Z80& cpu() { return cpu_; }
    uint8_t gpp() const { return gpp_; }
    uint8_t peek(uint16_t addr) { return memRead(addr); }

    // Snapshot of the whole machine: CPU board, terminal, floppy controllers,
    // and which disk images are mounted (by path; their contents stay in
    // the image files).
    std::vector<uint8_t> saveState();
    // Restores a snapshot taken with the same ROMs, remounting its disks.
    // On failure the machine is unchanged. `warnings` lists disks that
    // couldn't be remounted or have changed since the snapshot.
    bool loadState(const std::vector<uint8_t>& data, std::string* error = nullptr,
                   std::vector<std::string>* warnings = nullptr);

private:
    uint8_t memRead(uint16_t addr) override;
    void memWrite(uint16_t addr, uint8_t value) override;
    uint8_t ioRead(uint16_t port) override;
    void ioWrite(uint16_t port, uint8_t value) override;
    uint8_t intAck() override;

    uint8_t readPort(uint8_t port);
    void writeGpp(uint8_t value);
    void updateIrq();
    void drawTestPattern();
    uint64_t romHash() const;
    void saveDisks(StateWriter& w);
    void loadDisks(StateReader& r, std::vector<std::string>* warnings);
    bool loadStateUnchecked(const std::vector<uint8_t>& data, std::string* error,
                            std::vector<std::string>* warnings);

    MachineConfig config_;
    uint32_t ramTop_; // one past the last RAM address
    std::array<uint8_t, 65536> ram_{};
    std::array<uint8_t, 1024> h17Ram_{}; // H-88-1 controller RAM at 1400h
    Z80 cpu_;
    Ins8250 console_{kCpuHz};
    std::unique_ptr<H19> terminal_;
    std::unique_ptr<H17> h17_;
    std::unique_ptr<H37> h37_;
    int terminalLag_ = 0; // cycles the terminal is behind the CPU board
    uint8_t gpp_ = 0;
    int timerCountdown_ = kTimerCycles;
    bool timerPending_ = false;
    Framebuffer fb_;
    uint64_t frames_ = 0;
};

} // namespace h89
