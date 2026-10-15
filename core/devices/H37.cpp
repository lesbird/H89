#include "H37.h"

#include <algorithm>

namespace h89 {

namespace {

constexpr int ms(int milliseconds) { return H37::kCpuHz / 1000 * milliseconds; }

// Step rates for r1 r0 with the 1797 clocked at 1 MHz.
constexpr int kStepCycles[4] = {ms(6), ms(12), ms(20), ms(30)};
constexpr int kSettleCycles = ms(30);   // E flag / verify head settle at 1 MHz
constexpr int kIndexLimit = 5;          // revolutions before Record Not Found

} // namespace

void H37::reset() {
    now_ = 0;
    control_ = 0;
    interface_ = 0;
    status_ = 0;
    command_ = 0;
    track_ = 0;
    sector_ = 1;
    data_ = 0;
    intrq_ = drq_ = false;
    typeIStatus_ = true;
    interruptOnIndex_ = false;
    side_ = 0;
    stepDirection_ = 1;
    phase_ = Phase::Idle;
    buffer_.clear();
}

bool H37::insertDisk(int drive, const std::string& path, bool readOnly, std::string* error) {
    if (drive < 0 || drive >= kDrives) return false;
    SoftDisk disk;
    if (!disk.load(path, readOnly, error)) return false;
    drives_[drive].disk = std::move(disk);
    return true;
}

void H37::ejectDisk(int drive) {
    if (drive >= 0 && drive < kDrives) drives_[drive].disk = SoftDisk();
}

H37::Drive* H37::selected() {
    for (int d = 0; d < kDrives; ++d)
        if (control_ & (0x10 << d)) return &drives_[d];
    return nullptr;
}

// The disk turns (and so passes index holes and sectors) only with a
// selected drive holding a disk and the motors on.
bool H37::ready() {
    Drive* d = selected();
    return d && d->disk.loaded() && (control_ & CTL_MOTOR);
}

int H37::byteCycles() const { return kRevolutionCycles / trackBytes(); }

uint64_t H37::nextIndex(uint64_t after) const {
    return (after + kRevolutionCycles - 1) / kRevolutionCycles * kRevolutionCycles;
}

// Sectors are spread evenly around the track after a gap following the index.
int H37::idOffset(int index, int count) const {
    int gap = (control_ & CTL_MFM) ? 80 : 40;
    return gap + index * ((trackBytes() - gap) / std::max(count, 1));
}

int H37::dataOffset(int index, int count) const {
    return idOffset(index, count) + ((control_ & CTL_MFM) ? 44 : 24);
}

uint64_t H37::timeAt(int offset, uint64_t after) const {
    uint64_t t = after / kRevolutionCycles * kRevolutionCycles + static_cast<uint64_t>(offset) * byteCycles();
    while (t < after) t += kRevolutionCycles;
    return t;
}

const SoftDisk::Track* H37::currentTrack() {
    Drive* d = selected();
    if (!d || !d->disk.loaded()) return nullptr;
    const SoftDisk::Track* t = d->disk.track(d->cylinder, side_);
    // IDs written in the other density can't be decoded.
    if (!t || t->mfm != ((control_ & CTL_MFM) != 0)) return nullptr;
    return t;
}

uint8_t H37::read(int reg) {
    switch (reg) {
    case 2:
        if (interface_ & INTF_SECTOR_TRACK) return sector_;
        intrq_ = false;
        if (typeIStatus_) {
            uint8_t s = status_ & (ST_BUSY | ST_CRC | ST_SEEK_ERROR | ST_HEAD_LOADED);
            Drive* d = selected();
            if (d) {
                if (d->cylinder == 0) s |= ST_TRACK0;
                if (d->disk.loaded() && d->disk.readOnly()) s |= ST_WRITE_PROTECT;
            }
            if (ready() && now_ % kRevolutionCycles < kIndexPulseCycles) s |= ST_INDEX;
            return s;
        }
        return static_cast<uint8_t>((status_ & ~ST_DRQ) | (drq_ ? ST_DRQ : 0));
    case 3:
        if (interface_ & INTF_SECTOR_TRACK) return track_;
        setDrq(false);
        return data_;
    default:
        return 0xFF; // control and interface latches are write-only
    }
}

void H37::write(int reg, uint8_t value) {
    switch (reg) {
    case 0:
        control_ = value;
        break;
    case 1:
        interface_ = value;
        break;
    case 2:
        if (interface_ & INTF_SECTOR_TRACK) sector_ = value;
        else command(value);
        break;
    case 3:
        if (interface_ & INTF_SECTOR_TRACK) {
            track_ = value;
        } else {
            data_ = value;
            setDrq(false);
        }
        break;
    }
}

void H37::setDrq(bool on) { drq_ = on; }

void H37::command(uint8_t cmd) {
    if ((cmd & 0xF0) == 0xD0) {
        // Force Interrupt: stop whatever is running.
        bool wasBusy = status_ & ST_BUSY;
        phase_ = Phase::Idle;
        setDrq(false);
        status_ &= ~ST_BUSY;
        if (!wasBusy) {
            typeIStatus_ = true;
            status_ &= ~(ST_CRC | ST_SEEK_ERROR);
        }
        intrq_ = (cmd & 0x08) != 0;
        interruptOnIndex_ = (cmd & 0x04) != 0;
        lastIndexIrq_ = now_;
        return;
    }
    if (status_ & ST_BUSY) return; // ignored while busy, as on the chip
    command_ = cmd;
    intrq_ = false;
    setDrq(false);
    interruptOnIndex_ = false;
    if (!(cmd & 0x80)) {
        startTypeI(cmd);
        return;
    }
    typeIStatus_ = false;
    status_ = ST_BUSY;
    side_ = (cmd >> 1) & 1; // U: drives the SSO side select
    uint64_t start = now_ + ((cmd & 0x04) ? kSettleCycles : 0);
    startSearch(start);
    switch (cmd >> 4) {
    case 0x8: case 0x9: // Read Sector
    case 0xA: case 0xB: // Write Sector (write protect is checked once the ID is found)
        phase_ = Phase::FindSector;
        break;
    case 0xC: // Read Address
        searchId(Phase::AddressFound, Match::Any);
        break;
    case 0xE: // Read Track
        phase_ = Phase::ReadTrackWait;
        eventTime_ = ready() ? nextIndex(start) : UINT64_MAX;
        break;
    case 0xF: // Write Track (write protect is checked at the index hole)
        phase_ = Phase::WriteTrackWait;
        setDrq(true); // the first byte must be ready before the index hole
        eventTime_ = ready() ? nextIndex(start + 3 * byteCycles()) : UINT64_MAX;
        break;
    }
}

void H37::startTypeI(uint8_t cmd) {
    typeIStatus_ = true;
    status_ = ST_BUSY | ((cmd & 0x08) ? ST_HEAD_LOADED : (status_ & ST_HEAD_LOADED));
    int kind = cmd >> 4;
    if (kind == 0) { // Restore: seek to 0, believing we're at 255
        track_ = 0xFF;
        data_ = 0;
    }
    if (kind >= 4) stepDirection_ = kind < 6 ? 1 : -1; // Step In / Step Out; Step keeps the direction
    stepsLeft_ = 255;
    phase_ = Phase::Step;
    eventTime_ = now_;
}

void H37::stepOnce() {
    Drive* d = selected();
    if (d) d->cylinder = std::clamp(d->cylinder + stepDirection_, 0, 85);
}

void H37::finishTypeI() {
    if (command_ & 0x04) { // Verify, after the head settles
        phase_ = Phase::VerifySearch;
        eventTime_ = now_ + kSettleCycles;
        return;
    }
    finish(0);
}

void H37::startSearch(uint64_t from) {
    eventTime_ = from;
    searchDeadline_ = nextIndex(from) + static_cast<uint64_t>(kIndexLimit - 1) * kRevolutionCycles;
}

void H37::searchId(Phase found, Match match) {
    if (!ready()) {
        // No index pulses reach the 1797, so it never gives up.
        phase_ = Phase::NotFound;
        eventTime_ = UINT64_MAX;
        return;
    }
    const SoftDisk::Track* t = currentTrack();
    uint64_t best = UINT64_MAX;
    int bestIndex = -1;
    if (t) {
        int count = static_cast<int>(t->sectors.size());
        for (int i = 0; i < count; ++i) {
            const SoftDisk::Sector& s = t->sectors[i];
            if (match != Match::Any && s.cylinder != track_) continue;
            if (match == Match::TrackAndSector && (s.number != sector_ || s.data.empty())) continue;
            uint64_t when = timeAt(idOffset(i, count) + 7, eventTime_);
            if (when < best) best = when, bestIndex = i;
        }
    }
    if (bestIndex >= 0 && best <= searchDeadline_) {
        sectorIndex_ = bestIndex;
        phase_ = found;
        eventTime_ = best;
    } else {
        sectorIndex_ = -1;
        phase_ = Phase::NotFound;
        eventTime_ = std::max(searchDeadline_, eventTime_);
    }
}

void H37::finish(uint8_t errorBits) {
    phase_ = Phase::Idle;
    status_ = static_cast<uint8_t>((status_ & ~ST_BUSY) | errorBits);
    setDrq(false);
    intrq_ = true;
}

void H37::tick(int cycles) {
    now_ += static_cast<uint64_t>(cycles);
    while (phase_ != Phase::Idle && now_ >= eventTime_) process();
    if (interruptOnIndex_ && ready() && nextIndex(lastIndexIrq_ + 1) <= now_) {
        lastIndexIrq_ = now_;
        intrq_ = true;
    }
}

void H37::process() {
    Drive* d = selected();
    const SoftDisk::Track* t = currentTrack();
    switch (phase_) {
    case Phase::Idle:
        break;

    case Phase::Step: {
        int kind = command_ >> 4;
        if (kind <= 1) { // Restore, Seek
            if (kind == 0 && d && d->cylinder == 0) {
                track_ = 0;
                finishTypeI();
                return;
            }
            if (kind == 1 && track_ == data_) {
                finishTypeI();
                return;
            }
            if (--stepsLeft_ < 0) { // Restore never saw track 0
                finish(ST_SEEK_ERROR);
                return;
            }
            stepDirection_ = (kind == 0 || data_ < track_) ? -1 : 1;
            track_ = static_cast<uint8_t>(track_ + stepDirection_);
            stepOnce();
            eventTime_ = now_ + kStepCycles[command_ & 3];
            return;
        }
        // Step, Step In, Step Out: one step, updating the track register if u.
        if (command_ & 0x10) track_ = static_cast<uint8_t>(track_ + stepDirection_);
        stepOnce();
        phase_ = Phase::StepDone;
        eventTime_ = now_ + kStepCycles[command_ & 3];
        return;
    }

    case Phase::StepDone:
        finishTypeI();
        return;

    case Phase::VerifySearch:
        startSearch(now_);
        searchId(Phase::VerifyFound, Match::Track);
        return;

    case Phase::VerifyFound:
        finish(0);
        return;

    case Phase::NotFound:
        finish(typeIStatus_ ? ST_SEEK_ERROR : ST_NOT_FOUND);
        return;

    case Phase::FindSector:
        searchId(Phase::SectorFound, Match::TrackAndSector);
        return;

    case Phase::SectorFound: {
        const SoftDisk::Sector& s = t->sectors[sectorIndex_];
        uint64_t dataTime = timeAt(dataOffset(sectorIndex_, static_cast<int>(t->sectors.size())) + 1, now_);
        bufferPos_ = 0;
        if ((command_ & 0x20) && d && d->disk.readOnly()) {
            finish(ST_WRITE_PROTECT);
            return;
        }
        if (command_ & 0x20) { // Write: DRQ now; the first byte is due before the data mark
            buffer_.assign(s.data.size(), 0);
            setDrq(true);
            phase_ = Phase::WriteStart;
            eventTime_ = dataTime - 2 * static_cast<uint64_t>(byteCycles());
        } else {
            buffer_ = s.data;
            if (s.deleted) status_ |= ST_RECORD_TYPE;
            phase_ = Phase::ReadData;
            eventTime_ = dataTime;
        }
        return;
    }

    case Phase::ReadData:
        if (bufferPos_ < buffer_.size()) {
            if (drq_) status_ |= ST_LOST_DATA;
            data_ = buffer_[bufferPos_++];
            setDrq(true);
            eventTime_ += byteCycles();
            return;
        }
        if (t && sectorIndex_ >= 0 && t->sectors[sectorIndex_].crcError) {
            finish(ST_CRC);
            return;
        }
        if (command_ & 0x10) { // multiple: on to the next sector until none is found
            ++sector_;
            phase_ = Phase::FindSector;
            startSearch(now_ + 2 * byteCycles());
            return;
        }
        finish(0);
        return;

    case Phase::WriteStart:
        if (drq_) { // the CPU didn't supply the first byte in time
            finish(ST_LOST_DATA);
            return;
        }
        phase_ = Phase::WriteData;
        eventTime_ = now_;
        [[fallthrough]];

    case Phase::WriteData:
        if (bufferPos_ < buffer_.size()) {
            if (drq_) status_ |= ST_LOST_DATA; // write zeros for bytes that didn't come
            buffer_[bufferPos_++] = drq_ ? 0 : data_;
            if (bufferPos_ < buffer_.size()) setDrq(true);
            eventTime_ += byteCycles();
            return;
        }
        if (!d || !d->disk.writeSector(d->cylinder, side_, sectorIndex_, buffer_, command_ & 1)) {
            finish(ST_WRITE_PROTECT);
            return;
        }
        if (command_ & 0x10) {
            ++sector_;
            phase_ = Phase::FindSector;
            startSearch(now_ + 2 * byteCycles());
            return;
        }
        finish(0);
        return;

    case Phase::AddressFound: {
        const SoftDisk::Sector& s = t->sectors[sectorIndex_];
        buffer_ = {s.cylinder, s.head, s.number, s.sizeCode, 0, 0};
        bufferPos_ = 0;
        phase_ = Phase::ReadAddress;
        eventTime_ = now_;
        [[fallthrough]];
    }

    case Phase::ReadAddress:
        if (bufferPos_ < buffer_.size()) {
            if (drq_) status_ |= ST_LOST_DATA;
            data_ = buffer_[bufferPos_++];
            setDrq(true);
            eventTime_ += byteCycles();
            return;
        }
        sector_ = buffer_[0];
        finish(0);
        return;

    case Phase::ReadTrackWait: {
        // The raw bytes of the track as the head sees them.
        buffer_.assign(static_cast<size_t>(trackBytes()), (control_ & CTL_MFM) ? 0x4E : 0xFF);
        if (t) {
            int count = static_cast<int>(t->sectors.size());
            for (int i = 0; i < count; ++i) {
                const SoftDisk::Sector& s = t->sectors[i];
                size_t id = static_cast<size_t>(idOffset(i, count));
                const uint8_t idField[] = {0xFE, s.cylinder, s.head, s.number, s.sizeCode, 0, 0};
                for (size_t k = 0; k < sizeof idField && id + k < buffer_.size(); ++k) buffer_[id + k] = idField[k];
                size_t data = static_cast<size_t>(dataOffset(i, count));
                if (data < buffer_.size()) buffer_[data] = s.deleted ? 0xF8 : 0xFB;
                for (size_t k = 0; k < s.data.size() && data + 1 + k < buffer_.size(); ++k)
                    buffer_[data + 1 + k] = s.data[k];
            }
        }
        bufferPos_ = 0;
        phase_ = Phase::ReadTrack;
        return;
    }

    case Phase::ReadTrack:
        if (bufferPos_ < buffer_.size()) {
            if (drq_) status_ |= ST_LOST_DATA;
            data_ = buffer_[bufferPos_++];
            setDrq(true);
            eventTime_ += byteCycles();
            return;
        }
        finish(0);
        return;

    case Phase::WriteTrackWait:
        if (d && d->disk.readOnly()) {
            finish(ST_WRITE_PROTECT);
            return;
        }
        if (drq_) {
            finish(ST_LOST_DATA);
            return;
        }
        buffer_.clear();
        buffer_.push_back(data_);
        setDrq(true);
        phase_ = Phase::WriteTrack;
        eventTime_ = now_ + byteCycles();
        return;

    case Phase::WriteTrack:
        if (static_cast<int>(buffer_.size()) < trackBytes()) {
            if (drq_) status_ |= ST_LOST_DATA;
            buffer_.push_back(drq_ ? 0x00 : data_);
            if (static_cast<int>(buffer_.size()) < trackBytes()) setDrq(true);
            eventTime_ += byteCycles();
            return;
        }
        commitWriteTrack();
        finish(0);
        return;
    }
}

// Turns a Write Track byte stream into sectors: an ID mark (FE) and its four
// bytes, then a data mark (F8-FB) and the bytes up to the CRC (F7).
void H37::commitWriteTrack() {
    Drive* d = selected();
    if (!d || !d->disk.loaded()) return;
    SoftDisk::Track track;
    track.mfm = (control_ & CTL_MFM) != 0;
    const std::vector<uint8_t>& b = buffer_;
    size_t i = 0;
    while (i < b.size()) {
        if (b[i] != 0xFE || i + 5 > b.size()) {
            ++i;
            continue;
        }
        SoftDisk::Sector s;
        s.cylinder = b[i + 1];
        s.head = b[i + 2];
        s.number = b[i + 3];
        s.sizeCode = b[i + 4];
        i += 5;
        // Data mark before the next ID mark.
        while (i < b.size() && b[i] != 0xFE && (b[i] < 0xF8 || b[i] > 0xFB)) ++i;
        if (i < b.size() && b[i] != 0xFE) {
            s.deleted = b[i] != 0xFB;
            size_t start = ++i;
            while (i < b.size() && b[i] != 0xF7) ++i;
            s.data.assign(b.begin() + start, b.begin() + i);
        }
        track.sectors.push_back(std::move(s));
    }
    d->disk.formatTrack(d->cylinder, side_, std::move(track));
}

} // namespace h89
