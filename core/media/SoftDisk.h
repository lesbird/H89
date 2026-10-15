#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace h89 {

// A soft-sectored 5.25" disk (H37 and other WD179x controllers): each
// track holds whatever sectors it was formatted with, each with its own ID
// field and density.
//
// Two image formats are read and written:
//  - ImageDisk (.imd): any geometry; rewritten whole on every change.
//  - Raw sector dumps (.h37 and others): cylinders x sides x sectors in
//    order, sectors numbered from 1, with an optional 32-byte trailer such as
//    "SPT=10 SSZ=0256 TRK=40 SID=1 FM". Without a trailer the geometry is
//    guessed from the size. Reformatting to another geometry is kept in
//    memory until every track has the new one; then the image is rewritten
//    with a trailer describing it.
class SoftDisk {
public:
    struct Sector {
        uint8_t cylinder = 0, head = 0, number = 1; // ID field
        uint8_t sizeCode = 1;                       // ID field length code (IBM: 128 << n)
        bool deleted = false;                       // deleted data address mark
        bool crcError = false;                      // data recorded with a CRC error
        std::vector<uint8_t> data;
    };
    struct Track {
        bool mfm = false;
        std::vector<Sector> sectors; // in rotational order
    };

    bool load(const std::string& path, bool readOnly, std::string* error = nullptr);
    // In-memory disks (for tests): raw sector images or ImageDisk files.
    bool loadRaw(std::vector<uint8_t> bytes, int cylinders, int sides, int sectors, int sectorSize, bool mfm,
                 bool readOnly);
    bool loadImd(const std::vector<uint8_t>& bytes, bool readOnly, std::string* error = nullptr);

    bool loaded() const { return !tracks_.empty(); }
    bool readOnly() const { return readOnly_; }
    const std::string& path() const { return path_; }
    int cylinders() const { return cylinders_; }
    int sides() const { return sides_; }

    // The track under a head, or null (unformatted / beyond the disk).
    const Track* track(int cylinder, int head) const;
    // Replaces a sector's data and writes the image back.
    bool writeSector(int cylinder, int head, int index, const std::vector<uint8_t>& data, bool deleted);
    // Replaces a whole track (Write Track / format) and writes the image back.
    bool formatTrack(int cylinder, int head, Track track);

    // FNV-1a hash of every track's layout and data.
    uint64_t contentHash() const;

    // The image as ImageDisk / raw bytes (raw fails unless the geometry is uniform).
    std::vector<uint8_t> imdBytes() const;
    bool rawBytes(std::vector<uint8_t>* out) const;

private:
    struct Geometry {
        int sectors = 0, sectorSize = 0;
        bool mfm = false;
    };
    bool save();
    Track* mutableTrack(int cylinder, int head);

    std::vector<Track> tracks_; // cylinder * sides_ + head
    int cylinders_ = 0, sides_ = 1;
    bool readOnly_ = false;
    bool imd_ = false;
    std::string path_;
    std::string imdHeader_;     // "IMD 1.18: ..." line and comment, up to the 1Ah
    Geometry rawGeometry_;
    bool rawTrailer_ = false;   // raw image ends with a geometry trailer
};

} // namespace h89
