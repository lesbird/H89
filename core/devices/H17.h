#pragma once

#include "StateFile.h"
#include "media/H17Disk.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace h89 {

// H-88-1 hard-sectored floppy controller with up to three H17 drives, at
// ports 7Ch-7Fh:
//   7Ch  R/W  USRT data
//   7Dh  R    USRT status: bit 0 receive data available, bit 7 transmit empty
//        W    fill character
//   7Eh  W    sync character;  R  starts a search for sync
//   7Fh  R    disk status: bit 0 hole, 1 track 0, 2 write protect, 3 sync found
//        W    control: bit 0 write gate, 1-3 drive select, 4 motor,
//             5 step direction (1 = in), 6 step pulse
//
// The disk is modelled byte by byte: each track is a rotating stream of 10
// sectors, each starting at its sector hole with a gap, a header (sync,
// volume, track, sector, checksum), another gap and the data block (sync,
// 256 bytes, checksum). The USRT only finds sync at those sync marks.
class H17 {
public:
    static constexpr int kDrives = 3;
    static constexpr int kCyclesPerByte = 128;  // 128 kbit/s at 2.048 MHz
    static constexpr int kBytesPerSector = 320; // 20 ms per sector at 300 RPM
    static constexpr int kSectorCycles = kBytesPerSector * kCyclesPerByte;
    static constexpr int kRevolutionCycles = H17Disk::kSectorsPerTrack * kSectorCycles;
    static constexpr int kHoleCycles = 4096;    // 2 ms hole pulse
    static constexpr int kHeaderSyncByte = 10;  // byte offsets from the sector hole
    static constexpr int kDataSyncByte = 25;
    static constexpr uint8_t kSync = 0xFD;

    H17() { reset(); }
    void reset();

    void tick(int cycles) { now_ += cycles; }
    uint8_t read(int reg);
    void write(int reg, uint8_t value);

    bool insertDisk(int drive, const std::string& path, bool readOnly, std::string* error = nullptr);
    void ejectDisk(int drive);
    H17Disk& disk(int drive) { return drives_[drive].disk; }
    int headTrack(int drive) const { return drives_[drive].track; }
    bool motorOn() const { return control_ & CTL_MOTOR; }
    int selectedDrive() const;

    // Controller and head state; the disks themselves are saved by path.
    void saveState(StateWriter& w) const;
    void loadState(StateReader& r);

    // H17 checksum: rotate left after XOR, starting from 0 after the sync byte.
    static uint8_t checksum(const uint8_t* bytes, int count, uint8_t start = 0);

private:
    enum : uint8_t {
        CTL_WRITE_GATE = 0x01, CTL_MOTOR = 0x10, CTL_DIR_IN = 0x20, CTL_STEP = 0x40,
        ST_HOLE = 0x01, ST_TRACK0 = 0x02, ST_WRITE_PROTECT = 0x04, ST_SYNC = 0x08,
    };

    struct Drive {
        H17Disk disk;
        int track = 0;
    };

    bool spinning() const;
    bool holeActive() const;
    uint8_t streamByte(int index); // byte `index` of the selected track's stream
    void buildTrack();
    void startSyncSearch();
    void commitWrite();

    std::array<Drive, kDrives> drives_;
    uint64_t now_ = 0;
    uint8_t control_ = 0;

    // Receiver: after a sync search, bytes arrive from stream position
    // syncIndex_ starting at syncTime_.
    bool searching_ = false;
    uint64_t syncTime_ = 0;
    int syncIndex_ = 0;
    int64_t lastRead_ = -1;

    // Transmitter: bytes written while the write gate is on.
    uint64_t txBusyUntil_ = 0;
    uint64_t writeStart_ = 0;
    uint64_t writeFirst_ = 0; // when the first byte of writeBuffer_ went out
    std::vector<uint8_t> writeBuffer_;

    // Cached byte stream of the selected track.
    std::vector<uint8_t> trackStream_;
    int streamDrive_ = -1, streamTrack_ = -1;
};

} // namespace h89
