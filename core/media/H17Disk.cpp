#include "H17Disk.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace h89 {

bool H17Disk::load(const std::string& path, bool readOnly, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
    if (data.empty() || data.size() % kTrackSize != 0) {
        if (error) *error = path + " is not an H17 image (size must be a multiple of 2560)";
        return false;
    }
    loadData(std::move(data), readOnly);
    path_ = path;
    return true;
}

void H17Disk::loadData(std::vector<uint8_t> data, bool readOnly) {
    data_ = std::move(data);
    readOnly_ = readOnly;
    path_.clear();
    volumeOverride_.clear();
}

const uint8_t* H17Disk::sector(int track, int sector) const {
    if (track < 0 || track >= tracks() || sector < 0 || sector >= kSectorsPerTrack) return nullptr;
    return &data_[(track * kSectorsPerTrack + sector) * kSectorSize];
}

bool H17Disk::writeSector(int track, int sector, const uint8_t* bytes) {
    if (readOnly_ || track < 0 || track >= tracks() || sector < 0 || sector >= kSectorsPerTrack)
        return false;
    size_t offset = (track * kSectorsPerTrack + sector) * kSectorSize;
    std::memcpy(&data_[offset], bytes, kSectorSize);
    if (!path_.empty()) {
        std::fstream f(path_, std::ios::binary | std::ios::in | std::ios::out);
        if (!f) return false;
        f.seekp(static_cast<std::streamoff>(offset));
        f.write(reinterpret_cast<const char*>(bytes), kSectorSize);
    }
    return true;
}

bool H17Disk::hasHdosLabel() const {
    if (data_.size() < 10 * kSectorSize) return false;
    const uint8_t* label = &data_[9 * kSectorSize];
    int sectors = label[0x0C] | (label[0x0D] << 8);
    int sectorSize = label[0x0E] | (label[0x0F] << 8);
    return sectorSize == kSectorSize && sectors == tracks() * kSectorsPerTrack;
}

uint8_t H17Disk::volume(int track) const {
    auto it = volumeOverride_.find(track);
    if (it != volumeOverride_.end()) return it->second;
    // CP/M and other non-HDOS disks are formatted with volume 0 throughout.
    if (track == 0 || !hasHdosLabel()) return 0;
    return data_[9 * kSectorSize];
}

uint64_t H17Disk::contentHash() const {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : data_) h = (h ^ b) * 0x100000001b3ull;
    return h;
}

} // namespace h89
