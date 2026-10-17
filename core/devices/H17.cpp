#include "H17.h"

#include <algorithm>

namespace h89 {

namespace {
constexpr int kStreamBytes = H17Disk::kSectorsPerTrack * H17::kBytesPerSector;
}

uint8_t H17::checksum(const uint8_t* bytes, int count, uint8_t start) {
    uint8_t d = start;
    for (int i = 0; i < count; ++i) {
        d ^= bytes[i];
        d = static_cast<uint8_t>((d << 1) | (d >> 7));
    }
    return d;
}

void H17::reset() {
    for (auto& d : drives_) d.track = 0;
    control_ = 0;
    searching_ = false;
    lastRead_ = -1;
    txBusyUntil_ = 0;
    writeBuffer_.clear();
    streamDrive_ = streamTrack_ = -1;
}

bool H17::insertDisk(int drive, const std::string& path, bool readOnly, std::string* error) {
    if (drive < 0 || drive >= kDrives) return false;
    bool ok = drives_[drive].disk.load(path, readOnly, error);
    streamDrive_ = -1;
    return ok;
}

void H17::ejectDisk(int drive) {
    if (drive < 0 || drive >= kDrives) return;
    drives_[drive].disk = H17Disk{};
    streamDrive_ = -1;
}

int H17::selectedDrive() const {
    if (control_ & 0x02) return 0;
    if (control_ & 0x04) return 1;
    if (control_ & 0x08) return 2;
    return -1;
}

bool H17::spinning() const {
    int d = selectedDrive();
    return d >= 0 && (control_ & CTL_MOTOR) && drives_[d].disk.loaded();
}

bool H17::holeActive() const {
    if (!spinning()) return false;
    uint64_t pos = now_ % kRevolutionCycles;
    if (pos % kSectorCycles < kHoleCycles) return true; // sector hole
    // Index hole, halfway between the holes of sectors 9 and 0
    uint64_t index = 9 * kSectorCycles + kSectorCycles / 2;
    return pos >= index && pos < index + kHoleCycles;
}

void H17::buildTrack() {
    int d = selectedDrive();
    int t = d >= 0 ? drives_[d].track : -1;
    if (d == streamDrive_ && t == streamTrack_) return;
    streamDrive_ = d;
    streamTrack_ = t;
    trackStream_.assign(kStreamBytes, 0);
    if (d < 0) return;
    const H17Disk& disk = drives_[d].disk;
    for (int s = 0; s < H17Disk::kSectorsPerTrack; ++s) {
        const uint8_t* data = disk.sector(t, s);
        if (!data) continue; // beyond the recorded tracks: no sync marks
        uint8_t* p = &trackStream_[s * kBytesPerSector];
        uint8_t header[3] = {disk.volume(t), static_cast<uint8_t>(t), static_cast<uint8_t>(s)};
        p[kHeaderSyncByte] = kSync;
        std::copy(header, header + 3, p + kHeaderSyncByte + 1);
        p[kHeaderSyncByte + 4] = checksum(header, 3);
        p[kDataSyncByte] = kSync;
        std::copy(data, data + H17Disk::kSectorSize, p + kDataSyncByte + 1);
        p[kDataSyncByte + 1 + H17Disk::kSectorSize] = checksum(data, H17Disk::kSectorSize);
    }
}

uint8_t H17::streamByte(int index) {
    buildTrack();
    return trackStream_[index % kStreamBytes];
}

void H17::startSyncSearch() {
    searching_ = true;
    lastRead_ = -1;
    buildTrack();
    // Find the next sync mark that passes the head after now.
    uint64_t revStart = now_ - now_ % kRevolutionCycles;
    for (int rev = 0; rev < 2; ++rev) {
        for (int s = 0; s < H17Disk::kSectorsPerTrack; ++s) {
            for (int mark : {kHeaderSyncByte, kDataSyncByte}) {
                int index = s * kBytesPerSector + mark;
                if (trackStream_[index] != kSync) continue;
                uint64_t t = revStart + uint64_t(rev) * kRevolutionCycles + uint64_t(index) * kCyclesPerByte;
                if (t > now_) {
                    syncTime_ = t;
                    syncIndex_ = index;
                    return;
                }
            }
        }
    }
    searching_ = false; // nothing recorded on this track
}

uint8_t H17::read(int reg) {
    switch (reg & 3) {
    case 0: { // data
        if (!searching_ || !spinning() || now_ < syncTime_) return 0;
        int64_t idx = static_cast<int64_t>((now_ - syncTime_) / kCyclesPerByte);
        lastRead_ = idx;
        return streamByte(static_cast<int>(syncIndex_ + idx));
    }
    case 1: { // USRT status
        uint8_t st = 0;
        if (searching_ && spinning() && now_ >= syncTime_ &&
            static_cast<int64_t>((now_ - syncTime_) / kCyclesPerByte) > lastRead_)
            st |= 0x01;
        if (txBusyUntil_ <= now_ + kCyclesPerByte) st |= 0x80;
        return st;
    }
    case 2: // reading 176Q arms the receiver to search for sync
        startSyncSearch();
        return 0;
    default: { // disk status
        uint8_t st = 0;
        int d = selectedDrive();
        if (holeActive()) st |= ST_HOLE;
        if (d >= 0 && drives_[d].track == 0) st |= ST_TRACK0;
        if (d < 0 || !drives_[d].disk.loaded() || drives_[d].disk.readOnly()) st |= ST_WRITE_PROTECT;
        if (searching_ && spinning() && now_ >= syncTime_) st |= ST_SYNC;
        return st;
    }
    }
}

void H17::write(int reg, uint8_t value) {
    switch (reg & 3) {
    case 0: // data
        if (control_ & CTL_WRITE_GATE) {
            if (writeBuffer_.empty()) writeFirst_ = std::max(txBusyUntil_, now_);
            writeBuffer_.push_back(value);
            txBusyUntil_ = std::max(txBusyUntil_, now_) + kCyclesPerByte;
        }
        break;
    case 3: { // control
        uint8_t prev = control_;
        control_ = value;
        int d = selectedDrive();
        if (!(prev & CTL_STEP) && (value & CTL_STEP) && d >= 0) {
            Drive& drv = drives_[d];
            int maxTrack = std::max(39, drv.disk.tracks() - 1);
            drv.track = std::clamp(drv.track + ((value & CTL_DIR_IN) ? 1 : -1), 0, maxTrack);
        }
        if (!(prev & CTL_WRITE_GATE) && (value & CTL_WRITE_GATE)) {
            writeBuffer_.clear();
            writeStart_ = now_;
            searching_ = false;
        } else if ((prev & CTL_WRITE_GATE) && !(value & CTL_WRITE_GATE)) {
            commitWrite();
        }
        break;
    }
    default: // fill and sync character registers: the format is fixed here
        break;
    }
}

// Turns the bytes written during one write-gate period back into sectors.
// A normal sector write is gap + sync + 256 data bytes + checksum; formatting
// also writes the header (sync, volume, track, sector, checksum) first.
void H17::commitWrite() {
    int d = selectedDrive();
    if (d < 0 || !drives_[d].disk.loaded()) return;
    Drive& drv = drives_[d];
    const auto& buf = writeBuffer_;
    const int n = static_cast<int>(buf.size());

    // Sector whose slot the write started in (sector writes begin just after
    // that sector's header).
    int sector = static_cast<int>((writeStart_ % kRevolutionCycles) / kSectorCycles);

    // Byte i went out i byte times after the first one (the transmitter
    // paces them). A header can only be where headers are, near the start
    // of a sector slot (a format write starts at the hole; Heath's FORMAT
    // turns the gate on for an erase well before it writes); a sector write
    // starts after the header, so whatever its data holds is data, even
    // bytes that look like a header (00 00 00 00 on track 0, say).
    const int startByte = static_cast<int>((writeFirst_ % kSectorCycles) / kCyclesPerByte);
    auto inHeaderPlace = [&](int at) {
        return (startByte + at) % kBytesPerSector < (kHeaderSyncByte + kDataSyncByte) / 2;
    };

    int i = 0;
    while (i < n) {
        while (i < n && buf[i] != kSync) ++i;
        if (i >= n) break;
        const int rec = i + 1; // first byte after the sync
        bool header = inHeaderPlace(i) && rec + 3 < n && buf[rec + 1] == drv.track &&
                      buf[rec + 2] < H17Disk::kSectorsPerTrack && checksum(&buf[rec], 3) == buf[rec + 3];
        if (header) {
            sector = buf[rec + 2];
            if (buf[rec] != drv.disk.volume(drv.track)) drv.disk.setVolume(drv.track, buf[rec]);
            i = rec + 4;
        } else if (rec + H17Disk::kSectorSize <= n) {
            drv.disk.writeSector(drv.track, sector, &buf[rec]);
            i = rec + H17Disk::kSectorSize + 1;
        } else {
            break;
        }
    }
    writeBuffer_.clear();
    streamDrive_ = -1; // rebuild the stream with the new contents
}

void H17::saveState(StateWriter& w) const {
    w.begin("H17 ");
    for (const Drive& d : drives_) {
        w.i32(d.track);
        w.u32(static_cast<uint32_t>(d.disk.volumeOverrides().size()));
        for (auto [track, volume] : d.disk.volumeOverrides()) w.i32(track), w.u8(volume);
    }
    w.u64(now_), w.u8(control_);
    w.boolean(searching_), w.u64(syncTime_), w.i32(syncIndex_), w.u64(static_cast<uint64_t>(lastRead_));
    w.u64(txBusyUntil_), w.u64(writeStart_);
    w.bytes(writeBuffer_);
    w.end();
}

void H17::loadState(StateReader& r) {
    if (!r.begin("H17 ")) return;
    for (Drive& d : drives_) {
        d.track = r.i32();
        std::map<int, uint8_t> volumes;
        for (uint32_t n = r.u32(); n > 0 && r.ok(); --n) {
            int track = r.i32();
            volumes[track] = r.u8();
        }
        d.disk.setVolumeOverrides(std::move(volumes));
    }
    now_ = r.u64(), control_ = r.u8();
    searching_ = r.boolean(), syncTime_ = r.u64(), syncIndex_ = r.i32(), lastRead_ = static_cast<int64_t>(r.u64());
    txBusyUntil_ = r.u64(), writeStart_ = r.u64();
    writeBuffer_ = r.bytes();
    streamDrive_ = streamTrack_ = -1; // rebuild the cached track stream
    r.end();
}

} // namespace h89
