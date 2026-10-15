#pragma once

#include "H17Disk.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace h89 {

// One file in a disk image's directory.
struct DiskFile {
    std::string name;  // "PIP.ABS"
    uint32_t size = 0; // bytes, a whole number of sectors (HDOS) or records (CP/M)
    std::string date;  // "14-Oct-86", or empty when the filesystem keeps no dates
    std::string flags; // HDOS "SLWC", CP/M "RS" (read-only, system)
    int user = 0;      // CP/M user area
    bool protectedFile = false; // locked or write protected: cannot be deleted or replaced
};

// The files inside an H17 disk image, for copying them to and from the host.
// Understands HDOS 2.0 volumes and Heath CP/M 2.2 (40-track, single-sided)
// disks. Changes go through H17Disk::writeSector, so they reach the image
// file at once and a disk mounted in a drive stays consistent.
//
// The emulated operating system does not know about these changes: HDOS
// keeps a mounted disk's tables in memory (DISMOUNT and MOUNT it again) and
// CP/M remembers the allocation map until a warm boot (^C).
class DiskFileSystem {
public:
    enum class Format { Hdos, Cpm };

    // Detects the filesystem on `disk`; returns null (and sets `error`) if
    // it is not one we understand. `disk` must outlive the result.
    static std::unique_ptr<DiskFileSystem> open(H17Disk& disk, std::string* error = nullptr);
    virtual ~DiskFileSystem() = default;

    virtual Format format() const = 0;
    virtual std::string description() const = 0; // "HDOS volume 042: TEST DISK"
    virtual std::vector<DiskFile> list() const = 0;
    virtual uint32_t freeBytes() const = 0;

    // Converts a host file name ("readme.txt") to a valid name on this disk
    // ("README.TXT"); returns empty (and sets `error`) if it can't.
    virtual std::string diskName(const std::string& hostName, std::string* error = nullptr) const = 0;
    // Finds `name` (a disk name) in user area 0 / the volume.
    bool find(const std::string& name, DiskFile* file = nullptr) const;

    virtual bool read(const DiskFile& file, std::vector<uint8_t>* data, std::string* error = nullptr) const = 0;
    // Writes `data` as `name` (from diskName()). An existing file of that name
    // is replaced only if `replace`. The last sector or record is padded with
    // 0 (HDOS) or ^Z (CP/M).
    virtual bool write(const std::string& name, const std::vector<uint8_t>& data, bool replace,
                       std::string* error = nullptr) = 0;
    virtual bool remove(const DiskFile& file, std::string* error = nullptr) = 0;

protected:
    explicit DiskFileSystem(H17Disk& disk) : disk_(disk) {}

    // Sectors numbered from 0 across the whole disk.
    const uint8_t* sector(int n) const {
        return disk_.sector(n / H17Disk::kSectorsPerTrack, n % H17Disk::kSectorsPerTrack);
    }
    bool writeSector(int n, const uint8_t* bytes) {
        return disk_.writeSector(n / H17Disk::kSectorsPerTrack, n % H17Disk::kSectorsPerTrack, bytes);
    }
    bool checkWritable(std::string* error) const;

    H17Disk& disk_;
};

} // namespace h89
