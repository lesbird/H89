#include "check.h"
#include "media/DiskFiles.h"
#include "media/H17Disk.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using h89::DiskFile;
using h89::DiskFileSystem;
using h89::H17Disk;

namespace {

constexpr size_t kImageSize = 40 * H17Disk::kTrackSize;

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 7 + seed);
    return v;
}

// A freshly formatted CP/M disk is all E5.
H17Disk blankCpm(bool readOnly = false) {
    H17Disk disk;
    disk.loadData(std::vector<uint8_t>(kImageSize, 0xE5), readOnly);
    return disk;
}

// A minimal HDOS volume laid out like INIT's: label in sector 9, a
// two-block directory at sectors 20-23, the GRT in sector 24, and groups
// 13-199 free.
H17Disk blankHdos() {
    std::vector<uint8_t> image(kImageSize, 0);
    auto sector = [&](int n) { return &image[static_cast<size_t>(n) * H17Disk::kSectorSize]; };
    uint8_t* label = sector(9);
    label[0] = 42;  // serial
    label[3] = 20;  // directory
    label[5] = 24;  // GRT
    label[7] = 2;   // sectors per group
    label[12] = 400 & 0xFF;
    label[13] = 400 >> 8;
    label[15] = 1; // 256-byte sectors
    std::memcpy(label + 0x11, "UNIT TEST", 9);
    for (int block : {20, 22}) {
        uint8_t* b = sector(block);
        for (int i = 0; i < 22; ++i) b[i * 23] = 0xFF;
        b[507] = 23;
        b[508] = static_cast<uint8_t>(block);
        b[510] = block == 20 ? 22 : 0;
    }
    uint8_t* grt = sector(24);
    for (int g = 1; g < 13; ++g) grt[g] = 0xFF;
    grt[0] = 13;
    for (int g = 13; g < 199; ++g) grt[g] = static_cast<uint8_t>(g + 1);
    for (int g = 200; g < 256; ++g) grt[g] = 0xFF;
    H17Disk disk;
    disk.loadData(std::move(image), false);
    return disk;
}

} // namespace

TEST_CASE("blank CP/M disk is empty") {
    H17Disk disk = blankCpm();
    auto fs = DiskFileSystem::open(disk);
    CHECK_EQ(fs != nullptr, true);
    CHECK_EQ(fs->format() == DiskFileSystem::Format::Cpm, true);
    CHECK_EQ(fs->list().size(), size_t{0});
    CHECK_EQ(fs->freeBytes(), 90u * 1024);
}

TEST_CASE("CP/M write, read back, replace and delete") {
    H17Disk disk = blankCpm();
    auto fs = DiskFileSystem::open(disk);
    std::vector<uint8_t> data = pattern(20000, 1); // 157 records, 20 blocks, 2 extents
    CHECK_EQ(fs->write("BIG.DAT", data, false), true);

    auto files = fs->list();
    CHECK_EQ(files.size(), size_t{1});
    CHECK_EQ(files[0].name, std::string("BIG.DAT"));
    CHECK_EQ(files[0].size, 157u * 128);
    CHECK_EQ(fs->freeBytes(), 70u * 1024);

    std::vector<uint8_t> back;
    CHECK_EQ(fs->read(files[0], &back), true);
    CHECK_EQ(back.size(), size_t{157 * 128});
    CHECK_EQ(std::equal(data.begin(), data.end(), back.begin()), true);
    CHECK_EQ(back.back(), 0x1A); // padded with ^Z

    std::string error;
    CHECK_EQ(fs->write("BIG.DAT", pattern(10, 2), false, &error), false);
    CHECK_EQ(fs->write("BIG.DAT", pattern(10, 2), true, &error), true);
    CHECK_EQ(fs->list().size(), size_t{1});
    CHECK_EQ(fs->list()[0].size, 128u);
    CHECK_EQ(fs->freeBytes(), 89u * 1024);

    CHECK_EQ(fs->remove(fs->list()[0]), true);
    CHECK_EQ(fs->list().size(), size_t{0});
    CHECK_EQ(fs->freeBytes(), 90u * 1024);
}

TEST_CASE("CP/M rejects files that don't fit") {
    H17Disk disk = blankCpm();
    auto fs = DiskFileSystem::open(disk);
    std::string error;
    CHECK_EQ(fs->write("HUGE.DAT", pattern(91 * 1024, 0), false, &error), false);
    CHECK_EQ(fs->list().size(), size_t{0});
    CHECK_EQ(fs->write("FITS.DAT", pattern(90 * 1024, 0), false, &error), true);
    CHECK_EQ(fs->freeBytes(), 0u);
}

TEST_CASE("CP/M survives reopening and keeps write protection") {
    H17Disk disk = blankCpm();
    DiskFileSystem::open(disk)->write("A.TXT", pattern(300, 3), false);
    auto fs = DiskFileSystem::open(disk);
    CHECK_EQ(fs->find("A.TXT"), true);

    H17Disk locked = blankCpm(true);
    std::string error;
    CHECK_EQ(DiskFileSystem::open(locked)->write("A.TXT", pattern(1, 0), false, &error), false);
    CHECK_EQ(error, std::string("The disk is write protected."));
}

TEST_CASE("CP/M names") {
    H17Disk disk = blankCpm();
    auto fs = DiskFileSystem::open(disk);
    CHECK_EQ(fs->diskName("/tmp/hello world.text"), std::string("HELLOWOR.TEX"));
    CHECK_EQ(fs->diskName("Makefile"), std::string("MAKEFILE"));
    CHECK_EQ(fs->diskName("1st.asm"), std::string("1ST.ASM"));
    CHECK_EQ(fs->diskName(".hidden"), std::string(""));
}

TEST_CASE("unformatted disk is not recognised") {
    H17Disk disk;
    disk.loadData(std::vector<uint8_t>(kImageSize, 0), false);
    std::string error;
    CHECK_EQ(DiskFileSystem::open(disk, &error) == nullptr, true);
    CHECK_EQ(error.empty(), false);
}

TEST_CASE("HDOS write, read back and delete") {
    H17Disk disk = blankHdos();
    auto fs = DiskFileSystem::open(disk);
    CHECK_EQ(fs != nullptr, true);
    CHECK_EQ(fs->format() == DiskFileSystem::Format::Hdos, true);
    CHECK_EQ(fs->description(), std::string("HDOS volume 042: UNIT TEST"));
    CHECK_EQ(fs->freeBytes(), 187u * 2 * 256);

    std::vector<uint8_t> data = pattern(1000, 5); // 4 sectors, 2 groups
    CHECK_EQ(fs->write("TEST.DAT", data, false), true);
    DiskFile file;
    CHECK_EQ(fs->find("TEST.DAT", &file), true);
    CHECK_EQ(file.size, 1024u);
    CHECK_EQ(file.date.empty(), false);
    CHECK_EQ(fs->freeBytes(), 185u * 2 * 256);

    std::vector<uint8_t> back;
    CHECK_EQ(fs->read(file, &back), true);
    CHECK_EQ(back.size(), size_t{1024});
    CHECK_EQ(std::equal(data.begin(), data.end(), back.begin()), true);
    CHECK_EQ(back.back(), 0);

    // Group chain and directory entry as HDOS writes them.
    const uint8_t* grt = disk.sector(2, 4);
    CHECK_EQ(grt[0], 15);  // free list now starts after the two groups taken
    CHECK_EQ(grt[13], 14);
    CHECK_EQ(grt[14], 0);
    const uint8_t* entry = disk.sector(2, 0);
    CHECK_EQ(entry[16], 13); // first group
    CHECK_EQ(entry[17], 14); // last group
    CHECK_EQ(entry[18], 2);  // sectors used in the last group

    CHECK_EQ(fs->remove(file), true);
    CHECK_EQ(fs->list().size(), size_t{0});
    CHECK_EQ(fs->freeBytes(), 187u * 2 * 256);
}

TEST_CASE("HDOS directory spans blocks and fills up") {
    H17Disk disk = blankHdos();
    auto fs = DiskFileSystem::open(disk);
    std::string error;
    for (int i = 0; i < 44; ++i)
        CHECK_EQ(fs->write("F" + std::to_string(i) + ".DAT", pattern(10, static_cast<uint8_t>(i)), false, &error),
                 true);
    CHECK_EQ(fs->list().size(), size_t{44});
    CHECK_EQ(fs->write("ONEMORE.DAT", pattern(10, 0), false, &error), false);
    CHECK_EQ(error, std::string("The directory is full."));
    DiskFile last;
    CHECK_EQ(fs->find("F43.DAT", &last), true);
    std::vector<uint8_t> back;
    CHECK_EQ(fs->read(last, &back), true);
    CHECK_EQ(back[9], pattern(10, 43)[9]);
}

TEST_CASE("HDOS rejects files that don't fit and protects locked files") {
    H17Disk disk = blankHdos();
    auto fs = DiskFileSystem::open(disk);
    std::string error;
    CHECK_EQ(fs->write("HUGE.DAT", pattern(187 * 512 + 1, 0), false, &error), false);
    CHECK_EQ(fs->write("FITS.DAT", pattern(187 * 512, 0), false, &error), true);
    CHECK_EQ(fs->freeBytes(), 0u);

    DiskFile file;
    fs->find("FITS.DAT", &file);
    file.protectedFile = true;
    CHECK_EQ(fs->remove(file, &error), false);
}

TEST_CASE("HDOS names") {
    H17Disk disk = blankHdos();
    auto fs = DiskFileSystem::open(disk);
    std::string error;
    CHECK_EQ(fs->diskName("my-file.text"), std::string("MYFILE.TEX"));
    CHECK_EQ(fs->diskName("README"), std::string("README"));
    CHECK_EQ(fs->diskName("1st.asm", &error), std::string(""));
    CHECK_EQ(error.empty(), false);
}

int main() { return check::runAll(); }
