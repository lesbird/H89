#pragma once

#include "StateFile.h"
#include "media/SoftDisk.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace h89 {

// Z-89-37 soft-sectored floppy controller (H37 drives): a WD1797 plus two
// latches, at ports 78h-7Bh:
//   78h  W  control: bit 0 INTRQ interrupt enable, 1 DRQ enable, 2 MFM
//           (double density), 3 motor on, 4-7 drive select 0-3
//   79h  W  interface: bit 0 = 1 makes 7Ah/7Bh the sector/track registers
//   7Ah  R/W status/command, or sector
//   7Bh  R/W data, or track
//
// INTRQ interrupts at level 4 (RST 4). DRQ is used to end a HALT: while DRQ
// interrupts are enabled the CPU waits in HALT for each byte and the
// interrupt controller answers the acknowledge with EI, so execution
// carries on after the HALT without calling a handler.
//
// The disk is modelled as a rotating track at 300 RPM with sectors evenly
// spaced; bytes pass the head at the FM (125 kbit/s) or MFM (250 kbit/s)
// rate and the controller only finds IDs recorded in the density selected.
class H37 {
public:
    static constexpr int kDrives = 4;
    static constexpr int kCpuHz = 2'048'000;
    static constexpr int kRevolutionCycles = kCpuHz / 5; // 300 RPM
    static constexpr int kIndexPulseCycles = kCpuHz / 250; // 4 ms

    H37() { reset(); }
    void reset();

    void tick(int cycles);
    uint8_t read(int reg);
    void write(int reg, uint8_t value);

    // INTRQ, gated by the control register (RST 4).
    bool irq() const { return intrq_ && (control_ & CTL_INTRQ_ENABLE); }
    // DRQ, gated by the control register: wakes the CPU from HALT.
    bool drqInterrupt() const { return drq_ && (control_ & CTL_DRQ_ENABLE); }
    // While DRQ interrupts are enabled the card blocks the other interrupt
    // levels so they can't disturb a transfer.
    bool blocksInterrupts() const { return control_ & CTL_DRQ_ENABLE; }

    bool insertDisk(int drive, const std::string& path, bool readOnly, std::string* error = nullptr);
    void ejectDisk(int drive);
    SoftDisk& disk(int drive) { return drives_[drive].disk; }
    int headCylinder(int drive) const { return drives_[drive].cylinder; }
    bool busy() const { return status_ & ST_BUSY; }

    // Controller and head state; the disks themselves are saved by path.
    void saveState(StateWriter& w) const;
    void loadState(StateReader& r);

private:
    enum : uint8_t {
        CTL_INTRQ_ENABLE = 0x01, CTL_DRQ_ENABLE = 0x02, CTL_MFM = 0x04, CTL_MOTOR = 0x08,
        INTF_SECTOR_TRACK = 0x01,
        ST_BUSY = 0x01, ST_INDEX = 0x02, ST_DRQ = 0x02, ST_TRACK0 = 0x04, ST_LOST_DATA = 0x04,
        ST_CRC = 0x08, ST_SEEK_ERROR = 0x10, ST_NOT_FOUND = 0x10, ST_HEAD_LOADED = 0x20,
        ST_RECORD_TYPE = 0x20, ST_WRITE_PROTECT = 0x40, ST_NOT_READY = 0x80,
    };
    enum class Phase {
        Idle,
        Step,          // Type I: next step (or done)
        StepDone,      // Type I: single step's time is up
        VerifySearch,  // Type I verify: settle done, look for an ID
        VerifyFound,
        FindSector,    // Type II: look for the ID
        SectorFound,   // Type II: ID passing the head
        ReadData,
        WriteStart,
        WriteData,
        AddressFound,  // Read Address: ID passing the head
        ReadAddress,
        ReadTrackWait,
        ReadTrack,
        WriteTrackWait,
        WriteTrack,
        NotFound,      // search limit reached
    };

    struct Drive {
        SoftDisk disk;
        int cylinder = 0;
    };

    Drive* selected();
    bool ready();
    int byteCycles() const;
    int trackBytes() const { return (control_ & CTL_MFM) ? 6250 : 3125; }
    uint64_t nextIndex(uint64_t after) const;
    // Rotational byte offsets of a sector's ID and data fields.
    int idOffset(int index, int count) const;
    int dataOffset(int index, int count) const;
    // Earliest time >= `after` that sector `index` of the current track
    // reaches the given byte offset.
    uint64_t timeAt(int offset, uint64_t after) const;
    const SoftDisk::Track* currentTrack();

    void command(uint8_t cmd);
    void startTypeI(uint8_t cmd);
    void finishTypeI();
    void stepOnce();
    enum class Match { Any, Track, TrackAndSector };
    // From eventTime_, looks for the next matching ID before the revolution
    // limit and schedules `found` when it passes the head; otherwise
    // NotFound at the limit, or nothing at all if the disk isn't turning.
    void searchId(Phase found, Match match);
    void startSearch(uint64_t from);
    void finish(uint8_t errorBits);
    void setDrq(bool on);
    void commitWriteTrack();
    void process();

    std::array<Drive, kDrives> drives_;
    uint64_t now_ = 0;
    uint8_t control_ = 0;
    uint8_t interface_ = 0;

    // WD1797 registers.
    uint8_t status_ = 0, command_ = 0, track_ = 0, sector_ = 0, data_ = 0;
    bool intrq_ = false, drq_ = false;
    bool typeIStatus_ = true; // status shows Type I bits (index, track 0, ...)
    bool interruptOnIndex_ = false;
    uint64_t lastIndexIrq_ = 0;
    int side_ = 0;            // SSO output
    int stepDirection_ = 1;   // +1 in, -1 out

    Phase phase_ = Phase::Idle;
    uint64_t eventTime_ = 0;
    uint64_t searchDeadline_ = 0;
    int stepsLeft_ = 0;
    int sectorIndex_ = -1;    // sector found by the last search
    std::vector<uint8_t> buffer_;
    size_t bufferPos_ = 0;
};

} // namespace h89
