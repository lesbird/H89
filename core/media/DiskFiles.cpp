#include "DiskFiles.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>

namespace h89 {

namespace {

constexpr int kSectorSize = H17Disk::kSectorSize;

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Splits a host path's last component into upper-case name and extension,
// keeping only characters `keep` accepts.
template <typename Keep>
void splitHostName(const std::string& hostName, Keep keep, std::string* name, std::string* ext) {
    std::string base = hostName.substr(hostName.find_last_of("/\\") + 1);
    size_t dot = base.find_last_of('.');
    std::string n = upper(base.substr(0, dot));
    std::string e = dot == std::string::npos ? "" : upper(base.substr(dot + 1));
    name->clear();
    ext->clear();
    for (char c : n)
        if (keep(c)) *name += c;
    for (char c : e)
        if (keep(c)) *ext += c;
}

std::string joinName(const std::string& name, const std::string& ext) {
    return ext.empty() ? name : name + "." + ext;
}

const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// ---------------------------------------------------------------------------
// HDOS 2.0
//
// Sector 9 holds the volume label: serial number, init date, the first
// sector of the directory and of the GRT, and sectors per group. Space is
// allocated in groups; the Group Reservation Table (one sector) chains each
// file's groups (entry = next group, 0 = last) and GRT[0] heads the chain of
// free groups. The directory is itself a file (DIRECT.SYS) of two-sector
// blocks: 22 entries of 23 bytes, then 0, 23, this block's sector and the
// next block's sector (0 at the end). An entry whose first byte is FE or FF
// is free.
class HdosFileSystem : public DiskFileSystem {
public:
    static constexpr int kEntrySize = 23;
    static constexpr int kEntriesPerBlock = 22;
    static constexpr int kBlockSize = 2 * kSectorSize;
    // Entry fields.
    static constexpr int kExt = 8, kFlags = 14, kFirstGroup = 16, kLastGroup = 17, kLastSectorCount = 18,
                         kCreated = 19, kAltered = 21;
    static constexpr uint8_t kFlagSystem = 0x80, kFlagLocked = 0x40, kFlagWriteProtect = 0x20,
                             kFlagContiguous = 0x10;

    explicit HdosFileSystem(H17Disk& disk) : DiskFileSystem(disk) {
        const uint8_t* label = sector(9);
        serial_ = label[0];
        dirSector_ = label[3] | (label[4] << 8);
        grtSector_ = label[5] | (label[6] << 8);
        spg_ = label[7];
        totalSectors_ = disk.tracks() * H17Disk::kSectorsPerTrack;
        std::string text(reinterpret_cast<const char*>(label + 0x11), 60);
        text = text.substr(0, text.find('\0'));
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
        label_ = text;
    }

    bool valid(std::string* error) const {
        if (spg_ < 1 || spg_ > 16 || dirSector_ + 1 >= totalSectors_ || grtSector_ >= totalSectors_ ||
            totalSectors_ / spg_ > 256) {
            setError(error, "The HDOS label on this disk is damaged.");
            return false;
        }
        return true;
    }

    Format format() const override { return Format::Hdos; }

    std::string description() const override {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%03d", serial_);
        return "HDOS volume " + std::string(buf) + (label_.empty() ? "" : ": " + label_);
    }

    std::vector<DiskFile> list() const override {
        std::vector<DiskFile> files;
        std::array<uint8_t, 256> grt = readGrt();
        forEachEntry([&](const uint8_t* e, int, int) {
            DiskFile f;
            f.name = entryName(e);
            std::vector<int> groups;
            if (chain(grt, e, &groups)) f.size = sectorCount(e, groups) * kSectorSize;
            f.date = formatDate(e[kCreated] | (e[kCreated + 1] << 8));
            uint8_t flags = e[kFlags];
            if (flags & kFlagSystem) f.flags += 'S';
            if (flags & kFlagLocked) f.flags += 'L';
            if (flags & kFlagWriteProtect) f.flags += 'W';
            if (flags & kFlagContiguous) f.flags += 'C';
            f.protectedFile = flags & (kFlagLocked | kFlagWriteProtect);
            files.push_back(std::move(f));
            return true;
        });
        return files;
    }

    uint32_t freeBytes() const override {
        return static_cast<uint32_t>(freeGroups(readGrt()).size()) * spg_ * kSectorSize;
    }

    std::string diskName(const std::string& hostName, std::string* error) const override {
        std::string name, ext;
        splitHostName(hostName, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; },
                      &name, &ext);
        if (name.empty() || !std::isalpha(static_cast<unsigned char>(name[0]))) {
            setError(error, "HDOS file names must start with a letter: " + hostName);
            return "";
        }
        return joinName(name.substr(0, 8), ext.substr(0, 3));
    }

    bool read(const DiskFile& file, std::vector<uint8_t>* data, std::string* error) const override {
        std::array<uint8_t, 256> grt = readGrt();
        bool found = false, ok = false;
        forEachEntry([&](const uint8_t* e, int, int) {
            if (entryName(e) != file.name) return true;
            found = true;
            std::vector<int> groups;
            if (!chain(grt, e, &groups)) return false;
            int count = sectorCount(e, groups);
            data->clear();
            for (int i = 0; i < count; ++i) {
                const uint8_t* s = sector(groups[i / spg_] * spg_ + i % spg_);
                if (!s) return false;
                data->insert(data->end(), s, s + kSectorSize);
            }
            ok = true;
            return false;
        });
        if (!found) setError(error, file.name + " is not on this disk.");
        else if (!ok) setError(error, file.name + " is damaged (its group chain is broken).");
        return ok;
    }

    bool write(const std::string& name, const std::vector<uint8_t>& data, bool replace,
               std::string* error) override {
        if (!checkWritable(error)) return false;
        std::array<uint8_t, 256> grt = readGrt();

        DiskFile existing;
        bool exists = find(name, &existing);
        if (exists && !replace) {
            setError(error, name + " already exists on this disk.");
            return false;
        }
        if (exists && existing.protectedFile) {
            setError(error, name + " is write protected on this disk.");
            return false;
        }
        int sectors = std::max<int>(1, static_cast<int>((data.size() + kSectorSize - 1) / kSectorSize));
        int groupsNeeded = (sectors + spg_ - 1) / spg_;
        int available = static_cast<int>(freeGroups(grt).size());
        int slots = freeSlots();
        if (exists) {
            available += static_cast<int>(existing.size / kSectorSize + spg_ - 1) / spg_;
            ++slots;
        }
        if (groupsNeeded > available) {
            setError(error, "Not enough room for " + name + ": it needs " + std::to_string(groupsNeeded * spg_) +
                                " sectors and " + std::to_string(available * spg_) + " are free.");
            return false;
        }
        if (slots == 0) {
            setError(error, "The directory is full.");
            return false;
        }
        if (exists && !remove(existing, error)) return false;
        grt = readGrt();

        // Take groups from the head of the free chain; they are already
        // linked in order, so only the last one needs terminating.
        std::vector<int> groups = freeGroups(grt);
        groups.resize(groupsNeeded);
        grt[0] = grt[groups.back()];
        grt[groups.back()] = 0;

        std::vector<uint8_t> padded(data);
        padded.resize(static_cast<size_t>(sectors) * kSectorSize, 0);
        for (int i = 0; i < sectors; ++i) {
            if (!writeSector(groups[i / spg_] * spg_ + i % spg_, &padded[static_cast<size_t>(i) * kSectorSize]))
                return writeFailed(error);
        }
        if (!writeSector(grtSector_, grt.data())) return writeFailed(error);

        std::array<uint8_t, kEntrySize> entry{};
        size_t dot = name.find('.');
        std::string base = name.substr(0, dot), ext = dot == std::string::npos ? "" : name.substr(dot + 1);
        std::memcpy(&entry[0], base.data(), std::min<size_t>(base.size(), 8));
        std::memcpy(&entry[kExt], ext.data(), std::min<size_t>(ext.size(), 3));
        entry[13] = 3; // cluster factor: what HDOS 2.0 writes on H17 disks
        entry[kFirstGroup] = static_cast<uint8_t>(groups.front());
        entry[kLastGroup] = static_cast<uint8_t>(groups.back());
        entry[kLastSectorCount] = static_cast<uint8_t>(sectors - (groupsNeeded - 1) * spg_);
        uint16_t today = todayDate();
        entry[kCreated] = entry[kAltered] = today & 0xFF;
        entry[kCreated + 1] = entry[kAltered + 1] = today >> 8;

        bool written = false, failed = false;
        forEachSlot([&](uint8_t* block, int blockSector, int index) {
            if (block[index * kEntrySize] != 0xFE && block[index * kEntrySize] != 0xFF) return true;
            std::memcpy(block + index * kEntrySize, entry.data(), kEntrySize);
            failed = !writeBlock(blockSector, block);
            written = true;
            return false;
        });
        if (failed || !written) return writeFailed(error);
        return true;
    }

    bool remove(const DiskFile& file, std::string* error) override {
        if (!checkWritable(error)) return false;
        if (file.protectedFile) {
            setError(error, file.name + " is write protected on this disk.");
            return false;
        }
        std::array<uint8_t, 256> grt = readGrt();
        bool found = false, failed = false;
        forEachSlot([&](uint8_t* block, int blockSector, int index) {
            uint8_t* e = block + index * kEntrySize;
            if (e[0] == 0xFE || e[0] == 0xFF || entryName(e) != file.name) return true;
            found = true;
            std::vector<int> groups;
            if (chain(grt, e, &groups)) {
                // Return the chain to the front of the free list.
                grt[groups.back()] = grt[0];
                grt[0] = static_cast<uint8_t>(groups.front());
                failed = !writeSector(grtSector_, grt.data());
            }
            e[0] = 0xFE;
            failed = failed || !writeBlock(blockSector, block);
            return false;
        });
        if (!found) {
            setError(error, file.name + " is not on this disk.");
            return false;
        }
        return failed ? writeFailed(error) : true;
    }

private:
    static std::string entryName(const uint8_t* e) {
        auto field = [](const uint8_t* p, int n) {
            std::string s;
            for (int i = 0; i < n && p[i]; ++i) s += static_cast<char>(p[i]);
            return s;
        };
        return joinName(field(e, 8), field(e + kExt, 3));
    }

    static std::string formatDate(uint16_t d) {
        int day = d & 31, month = (d >> 5) & 15, year = (d >> 9) + 70;
        if (day == 0 || month < 1 || month > 12) return "";
        char buf[16];
        std::snprintf(buf, sizeof buf, "%02d-%s-%02d", day, kMonths[month - 1], year % 100);
        return buf;
    }

    // Today, except that HDOS 2.0 can't show years after 1999 ("15-Oct-"),
    // so later years become 1999.
    static uint16_t todayDate() {
        std::time_t now = std::time(nullptr);
        std::tm* t = std::localtime(&now);
        int year = std::clamp(t->tm_year - 70, 0, 99 - 70);
        return static_cast<uint16_t>((year << 9) | ((t->tm_mon + 1) << 5) | t->tm_mday);
    }

    std::array<uint8_t, 256> readGrt() const {
        std::array<uint8_t, 256> grt{};
        if (const uint8_t* s = sector(grtSector_)) std::memcpy(grt.data(), s, grt.size());
        return grt;
    }

    std::vector<int> freeGroups(const std::array<uint8_t, 256>& grt) const {
        std::vector<int> groups;
        for (int g = grt[0]; g != 0 && groups.size() < 256; g = grt[g]) groups.push_back(g);
        return groups;
    }

    // Follows an entry's groups from the first to the last.
    bool chain(const std::array<uint8_t, 256>& grt, const uint8_t* e, std::vector<int>* groups) const {
        int maxGroups = totalSectors_ / spg_;
        for (int g = e[kFirstGroup];; g = grt[g]) {
            if (g == 0 || g >= maxGroups || static_cast<int>(groups->size()) >= maxGroups) return false;
            groups->push_back(g);
            if (g == e[kLastGroup]) return true;
        }
    }

    int sectorCount(const uint8_t* e, const std::vector<int>& groups) const {
        int last = std::clamp<int>(e[kLastSectorCount], 1, spg_);
        return static_cast<int>(groups.size() - 1) * spg_ + last;
    }

    // Calls fn(block, blockSector, index) for each directory slot, in
    // directory order, until it returns false. `block` is a modifiable copy.
    template <typename Fn>
    void forEachSlot(Fn fn) const {
        std::vector<bool> seen(totalSectors_);
        for (int s = dirSector_; s > 0 && s + 1 < totalSectors_ && !seen[s];) {
            seen[s] = true;
            std::array<uint8_t, kBlockSize> block;
            std::memcpy(block.data(), sector(s), kSectorSize);
            std::memcpy(block.data() + kSectorSize, sector(s + 1), kSectorSize);
            for (int i = 0; i < kEntriesPerBlock; ++i)
                if (!fn(block.data(), s, i)) return;
            s = block[kBlockSize - 2] | (block[kBlockSize - 1] << 8);
        }
    }

    // Calls fn(entry, blockSector, index) for each file until it returns false.
    template <typename Fn>
    void forEachEntry(Fn fn) const {
        forEachSlot([&](uint8_t* block, int s, int i) {
            const uint8_t* e = block + i * kEntrySize;
            return e[0] == 0xFE || e[0] == 0xFF ? true : fn(e, s, i);
        });
    }

    int freeSlots() const {
        int n = 0;
        forEachSlot([&](uint8_t* block, int, int i) {
            n += block[i * kEntrySize] == 0xFE || block[i * kEntrySize] == 0xFF;
            return true;
        });
        return n;
    }

    bool writeBlock(int s, const uint8_t* block) {
        return writeSector(s, block) && writeSector(s + 1, block + kSectorSize);
    }

    static bool writeFailed(std::string* error) {
        setError(error, "Couldn't write to the disk image.");
        return false;
    }

    int serial_ = 0;
    int dirSector_ = 0;
    int grtSector_ = 0;
    int spg_ = 1;
    int totalSectors_ = 0;
    std::string label_;
};

// ---------------------------------------------------------------------------
// Heath CP/M 2.2 on a 40-track single-sided H17 disk
//
// Tracks 0-2 hold the system; the filesystem starts on track 3. Blocks are
// 1K (four 256-byte sectors) and the first two hold the 64-entry
// directory. The BIOS lays logical sectors on each track with a skew of 4.
class CpmFileSystem : public DiskFileSystem {
public:
    static constexpr int kTracks = 40;
    static constexpr int kOffset = 3;          // reserved tracks
    static constexpr int kBlockSize = 1024;
    static constexpr int kSectorsPerBlock = kBlockSize / kSectorSize;
    static constexpr int kBlocks = 92;         // DSM + 1
    static constexpr int kDirBlocks = 2;       // AL0 = C0h
    static constexpr int kDirEntries = 64;     // DRM + 1
    static constexpr int kEntrySize = 32;
    static constexpr int kRecordSize = 128;
    static constexpr int kRecordsPerExtent = 128;
    static constexpr int kBlocksPerEntry = 16; // 8-bit block numbers, EXM = 0
    static constexpr uint8_t kEmpty = 0xE5;
    static constexpr int kSkew[H17Disk::kSectorsPerTrack] = {0, 4, 8, 2, 6, 1, 5, 9, 3, 7};

    using Directory = std::array<uint8_t, kDirEntries * kEntrySize>;

    explicit CpmFileSystem(H17Disk& disk) : DiskFileSystem(disk) {}

    // A directory where every used entry looks like a CP/M file.
    bool valid() const {
        if (disk_.tracks() != kTracks) return false;
        Directory dir = readDirectory();
        for (int i = 0; i < kDirEntries; ++i) {
            const uint8_t* e = &dir[i * kEntrySize];
            if (e[0] == kEmpty) continue;
            if (e[0] > 15 || e[15] > kRecordsPerExtent) return false;
            for (int k = 1; k < 12; ++k) {
                uint8_t c = e[k] & 0x7F;
                if (c < 0x20 || c > 0x7E) return false;
            }
            for (int k = 16; k < 32; ++k)
                if (e[k] >= kBlocks) return false;
        }
        return true;
    }

    Format format() const override { return Format::Cpm; }
    std::string description() const override { return "CP/M 2.2 disk"; }

    std::vector<DiskFile> list() const override {
        Directory dir = readDirectory();
        std::map<std::pair<int, std::string>, DiskFile> files;
        for (int i = 0; i < kDirEntries; ++i) {
            const uint8_t* e = &dir[i * kEntrySize];
            if (e[0] > 15) continue;
            DiskFile& f = files[{e[0], entryName(e)}];
            if (f.name.empty()) {
                f.name = entryName(e);
                f.user = e[0];
                if (e[9] & 0x80) f.flags += 'R';
                if (e[10] & 0x80) f.flags += 'S';
                f.protectedFile = e[9] & 0x80;
            }
            f.size = std::max(f.size, static_cast<uint32_t>(extentNumber(e) * kRecordsPerExtent + e[15]) * kRecordSize);
        }
        std::vector<DiskFile> result;
        for (auto& [key, f] : files) result.push_back(f);
        return result;
    }

    uint32_t freeBytes() const override {
        std::vector<bool> used = usedBlocks(readDirectory());
        return static_cast<uint32_t>(std::count(used.begin(), used.end(), false)) * kBlockSize;
    }

    std::string diskName(const std::string& hostName, std::string* error) const override {
        std::string name, ext;
        splitHostName(hostName, [](char c) {
            return c > 0x20 && c < 0x7F && !std::strchr("<>.,;:=?*[]|()/\\\"", c);
        }, &name, &ext);
        if (name.empty()) {
            setError(error, "Can't make a CP/M file name from " + hostName);
            return "";
        }
        return joinName(name.substr(0, 8), ext.substr(0, 3));
    }

    bool read(const DiskFile& file, std::vector<uint8_t>* data, std::string* error) const override {
        Directory dir = readDirectory();
        std::vector<const uint8_t*> extents = fileEntries(dir, file.user, file.name);
        if (extents.empty()) {
            setError(error, file.name + " is not on this disk.");
            return false;
        }
        data->clear();
        for (const uint8_t* e : extents) {
            int records = static_cast<int>(extentNumber(e) * kRecordsPerExtent + e[15]) -
                          static_cast<int>(data->size() / kRecordSize);
            for (int r = 0; r < records; ++r) {
                int block = e[16 + r * kRecordSize / kBlockSize];
                if (block < kDirBlocks || block >= kBlocks) {
                    setError(error, file.name + " is damaged (it refers to a missing block).");
                    return false;
                }
                int offset = (r * kRecordSize) % kBlockSize;
                const uint8_t* s = sector(diskSector(block * kSectorsPerBlock + offset / kSectorSize));
                const uint8_t* rec = s + offset % kSectorSize;
                data->insert(data->end(), rec, rec + kRecordSize);
            }
        }
        return true;
    }

    bool write(const std::string& name, const std::vector<uint8_t>& data, bool replace,
               std::string* error) override {
        if (!checkWritable(error)) return false;
        Directory dir = readDirectory();
        std::vector<const uint8_t*> existing = fileEntries(dir, 0, name);
        if (!existing.empty()) {
            if (!replace) {
                setError(error, name + " already exists on this disk.");
                return false;
            }
            if (existing.front()[9] & 0x80) {
                setError(error, name + " is read-only on this disk.");
                return false;
            }
        }
        int records = static_cast<int>((data.size() + kRecordSize - 1) / kRecordSize);
        int blocks = (records * kRecordSize + kBlockSize - 1) / kBlockSize;
        int entries = std::max(1, (blocks + kBlocksPerEntry - 1) / kBlocksPerEntry);

        // Plan on a copy with the old file gone, so its space is reusable.
        for (const uint8_t* e : existing) dir[e - dir.data()] = kEmpty;
        std::vector<bool> used = usedBlocks(dir);
        std::vector<int> freeList;
        for (int b = 0; b < kBlocks; ++b)
            if (!used[b]) freeList.push_back(b);
        std::vector<int> freeEntries;
        for (int i = 0; i < kDirEntries; ++i)
            if (dir[i * kEntrySize] == kEmpty) freeEntries.push_back(i);
        if (blocks > static_cast<int>(freeList.size())) {
            setError(error, "Not enough room for " + name + ": it needs " + std::to_string(blocks) + "K and " +
                                std::to_string(freeList.size()) + "K is free.");
            return false;
        }
        if (entries > static_cast<int>(freeEntries.size())) {
            setError(error, "The directory is full.");
            return false;
        }

        std::vector<uint8_t> padded(data);
        padded.resize(static_cast<size_t>(blocks) * kBlockSize, 0x1A);
        for (int b = 0; b < blocks; ++b) {
            for (int s = 0; s < kSectorsPerBlock; ++s) {
                const uint8_t* bytes = &padded[(static_cast<size_t>(b) * kSectorsPerBlock + s) * kSectorSize];
                if (!writeSector(diskSector(freeList[b] * kSectorsPerBlock + s), bytes)) return writeFailed(error);
            }
        }

        size_t dot = name.find('.');
        std::string base = name.substr(0, dot), ext = dot == std::string::npos ? "" : name.substr(dot + 1);
        for (int k = 0; k < entries; ++k) {
            uint8_t* e = &dir[freeEntries[k] * kEntrySize];
            std::memset(e, 0, kEntrySize);
            std::memset(e + 1, ' ', 11);
            std::memcpy(e + 1, base.data(), std::min<size_t>(base.size(), 8));
            std::memcpy(e + 9, ext.data(), std::min<size_t>(ext.size(), 3));
            int inEntry = std::min(kRecordsPerExtent, records - k * kRecordsPerExtent);
            e[12] = static_cast<uint8_t>(k & 31); // EX
            e[14] = static_cast<uint8_t>(k >> 5); // S2
            e[15] = static_cast<uint8_t>(inEntry);
            for (int i = 0; i < kBlocksPerEntry && k * kBlocksPerEntry + i < blocks; ++i)
                e[16 + i] = static_cast<uint8_t>(freeList[k * kBlocksPerEntry + i]);
        }
        return writeDirectory(dir) ? true : writeFailed(error);
    }

    bool remove(const DiskFile& file, std::string* error) override {
        if (!checkWritable(error)) return false;
        if (file.protectedFile) {
            setError(error, file.name + " is read-only on this disk.");
            return false;
        }
        Directory dir = readDirectory();
        std::vector<const uint8_t*> extents = fileEntries(dir, file.user, file.name);
        if (extents.empty()) {
            setError(error, file.name + " is not on this disk.");
            return false;
        }
        for (const uint8_t* e : extents) dir[e - dir.data()] = kEmpty;
        return writeDirectory(dir) ? true : writeFailed(error);
    }

private:
    // Disk sector holding the filesystem's 256-byte logical sector `n`.
    static int diskSector(int n) {
        int track = kOffset + n / H17Disk::kSectorsPerTrack;
        return track * H17Disk::kSectorsPerTrack + kSkew[n % H17Disk::kSectorsPerTrack];
    }

    static constexpr int kDirSectors = kDirEntries * kEntrySize / kSectorSize;

    Directory readDirectory() const {
        Directory dir{};
        for (int s = 0; s < kDirSectors; ++s)
            if (const uint8_t* p = sector(diskSector(s))) std::memcpy(&dir[s * kSectorSize], p, kSectorSize);
        return dir;
    }

    // Writes back the directory sectors that changed.
    bool writeDirectory(const Directory& dir) {
        for (int s = 0; s < kDirSectors; ++s) {
            const uint8_t* old = sector(diskSector(s));
            if (old && std::memcmp(old, &dir[s * kSectorSize], kSectorSize) == 0) continue;
            if (!writeSector(diskSector(s), &dir[s * kSectorSize])) return false;
        }
        return true;
    }

    static std::string entryName(const uint8_t* e) {
        auto field = [](const uint8_t* p, int n) {
            std::string s;
            for (int i = 0; i < n; ++i) s += static_cast<char>(p[i] & 0x7F);
            while (!s.empty() && s.back() == ' ') s.pop_back();
            return s;
        };
        return joinName(field(e + 1, 8), field(e + 9, 3));
    }

    static int extentNumber(const uint8_t* e) { return (e[14] << 5) | (e[12] & 31); }

    // A file's directory entries in extent order.
    static std::vector<const uint8_t*> fileEntries(const Directory& dir, int user, const std::string& name) {
        std::vector<const uint8_t*> entries;
        for (int i = 0; i < kDirEntries; ++i) {
            const uint8_t* e = &dir[i * kEntrySize];
            if (e[0] == user && entryName(e) == name) entries.push_back(e);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const uint8_t* a, const uint8_t* b) { return extentNumber(a) < extentNumber(b); });
        return entries;
    }

    static std::vector<bool> usedBlocks(const Directory& dir) {
        std::vector<bool> used(kBlocks);
        for (int b = 0; b < kDirBlocks; ++b) used[b] = true;
        for (int i = 0; i < kDirEntries; ++i) {
            const uint8_t* e = &dir[i * kEntrySize];
            if (e[0] > 15) continue;
            for (int k = 16; k < 32; ++k)
                if (e[k] && e[k] < kBlocks) used[e[k]] = true;
        }
        return used;
    }

    static bool writeFailed(std::string* error) {
        setError(error, "Couldn't write to the disk image.");
        return false;
    }
};

} // namespace

std::unique_ptr<DiskFileSystem> DiskFileSystem::open(H17Disk& disk, std::string* error) {
    if (!disk.loaded()) {
        setError(error, "There is no disk in the drive.");
        return nullptr;
    }
    if (disk.hasHdosLabel()) {
        auto fs = std::make_unique<HdosFileSystem>(disk);
        if (!fs->valid(error)) return nullptr;
        return fs;
    }
    auto cpm = std::make_unique<CpmFileSystem>(disk);
    if (cpm->valid()) return cpm;
    setError(error, "This disk doesn't hold an HDOS or CP/M filesystem that can be read.");
    return nullptr;
}

bool DiskFileSystem::find(const std::string& name, DiskFile* file) const {
    for (const DiskFile& f : list()) {
        if (f.user == 0 && f.name == name) {
            if (file) *file = f;
            return true;
        }
    }
    return false;
}

bool DiskFileSystem::checkWritable(std::string* error) const {
    if (disk_.readOnly()) {
        setError(error, "The disk is write protected.");
        return false;
    }
    return true;
}

} // namespace h89
