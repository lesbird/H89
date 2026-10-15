#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace h89 {

// A hard-sectored 5.25" disk for the H17: 10 sectors of 256 bytes per track,
// stored as a raw .h8d image (track 0 sector 0, sector 1, ...). 40-track
// single-sided images are 102,400 bytes.
class H17Disk {
public:
    static constexpr int kSectorsPerTrack = 10;
    static constexpr int kSectorSize = 256;
    static constexpr int kTrackSize = kSectorsPerTrack * kSectorSize;

    // Loads an image; returns false (and sets `error`) on failure. Writes go
    // back to the file unless `readOnly`.
    bool load(const std::string& path, bool readOnly, std::string* error = nullptr);
    // An in-memory disk (for tests).
    void loadData(std::vector<uint8_t> data, bool readOnly);

    bool loaded() const { return !data_.empty(); }
    bool readOnly() const { return readOnly_; }
    int tracks() const { return static_cast<int>(data_.size() / kTrackSize); }
    const std::string& path() const { return path_; }

    const uint8_t* sector(int track, int sector) const;
    // Replaces a sector and writes it through to the file.
    bool writeSector(int track, int sector, const uint8_t* bytes);

    // Volume number recorded in the sector headers of `track`. HDOS keeps
    // the disk's volume in its label (track 0 sector 9, byte 0) and writes
    // it into the headers of every track except track 0; other systems use 0.
    uint8_t volume(int track) const;
    // True if track 0 sector 9 is an HDOS label consistent with this image.
    bool hasHdosLabel() const;
    void setVolume(int track, uint8_t volume) { volumeOverride_[track] = volume; }

private:
    std::vector<uint8_t> data_;
    std::string path_;
    bool readOnly_ = false;
    std::map<int, uint8_t> volumeOverride_; // from reformatted tracks
};

} // namespace h89
