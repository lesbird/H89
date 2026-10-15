#include "check.h"
#include "devices/H37.h"
#include "media/SoftDisk.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using h89::H37;
using h89::SoftDisk;

namespace {

// 40 x 10 x 256 FM with every byte of a sector = its number + cylinder.
std::vector<uint8_t> rawImage() {
    std::vector<uint8_t> raw;
    for (int c = 0; c < 40; ++c)
        for (int s = 1; s <= 10; ++s) raw.insert(raw.end(), 256, static_cast<uint8_t>(c * 16 + s));
    return raw;
}

constexpr uint8_t kMotor = 0x08, kMfm = 0x04, kDrive0 = 0x10, kIntEnable = 0x01, kDrqEnable = 0x02;

struct Rig {
    H37 fdc;
    Rig(bool mfm = false) {
        fdc.disk(0).loadRaw(rawImage(), 40, 1, 10, 256, mfm, false);
        fdc.write(0, kDrive0 | kMotor | kIntEnable | kDrqEnable | (mfm ? kMfm : 0));
    }
    void setSector(int s) {
        fdc.write(1, 1);
        fdc.write(2, static_cast<uint8_t>(s));
        fdc.write(1, 0);
    }
    void setTrack(int t) {
        fdc.write(1, 1);
        fdc.write(3, static_cast<uint8_t>(t));
        fdc.write(1, 0);
    }
    // Runs until INTRQ, reading (or supplying) bytes on DRQ. Returns the status.
    uint8_t run(std::vector<uint8_t>* in, const std::vector<uint8_t>* out = nullptr, double seconds = 3) {
        size_t next = 0;
        for (int i = 0; i < seconds * H37::kCpuHz / 32; ++i) {
            fdc.tick(32);
            if (fdc.drqInterrupt()) {
                if (out) fdc.write(3, next < out->size() ? (*out)[next++] : 0);
                else if (in) in->push_back(fdc.read(3));
            }
            if (fdc.irq()) return fdc.read(2);
        }
        return 0xFF; // timed out
    }
};

} // namespace

TEST_CASE("raw image survives ImageDisk round trip") {
    SoftDisk a;
    CHECK_EQ(a.loadRaw(rawImage(), 40, 1, 10, 256, false, false), true);
    SoftDisk b;
    CHECK_EQ(b.loadImd(a.imdBytes(), false), true);
    CHECK_EQ(b.cylinders(), 40);
    CHECK_EQ(b.sides(), 1);
    std::vector<uint8_t> raw;
    CHECK_EQ(b.rawBytes(&raw), false); // b has no raw geometry of its own
    CHECK_EQ(b.track(39, 0)->sectors[9].data[0], static_cast<uint8_t>(39 * 16 + 10));
    CHECK_EQ(b.track(0, 0)->mfm, false);
}

TEST_CASE("raw image with trailer loads and saves its geometry") {
    std::string path = "h37_test_image.h37";
    std::vector<uint8_t> file = rawImage();
    const char trailer[33] = "SPT=10 SSZ=0256 TRK=40 SID=1 FM";
    file.insert(file.end(), trailer, trailer + 32);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(file.data()),
                                                static_cast<std::streamsize>(file.size()));
    SoftDisk d;
    CHECK_EQ(d.load(path, false), true);
    CHECK_EQ(d.cylinders(), 40);
    std::vector<uint8_t> sector(256, 0xAB);
    CHECK_EQ(d.writeSector(5, 0, 2, sector, false), true);
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> back((std::istreambuf_iterator<char>(in)), {});
    CHECK_EQ(back.size(), file.size());
    CHECK_EQ(back[(5 * 10 + 2) * 256], 0xAB);
    CHECK_EQ(std::string(back.end() - 32, back.end() - 1), std::string(trailer));
    std::remove(path.c_str());
}

TEST_CASE("restore and seek move the head") {
    Rig r;
    r.fdc.write(2, 0x03); // Restore
    CHECK_EQ(r.run(nullptr) & 0x04, 0x04); // track 0
    r.fdc.write(3, 12);
    r.fdc.write(2, 0x17); // Seek with verify
    uint8_t status = r.run(nullptr);
    CHECK_EQ(status & 0x10, 0); // no seek error: IDs on cylinder 12 match
    CHECK_EQ(r.fdc.headCylinder(0), 12);
    r.fdc.write(2, 0x40); // Step In (no track update)
    r.run(nullptr);
    CHECK_EQ(r.fdc.headCylinder(0), 13);
}

TEST_CASE("read multiple sectors to the end of the track") {
    Rig r;
    r.fdc.write(2, 0x03);
    r.run(nullptr);
    r.setSector(1);
    std::vector<uint8_t> data;
    r.fdc.write(2, 0x9C); // Read multiple, IBM sizes, settle
    uint8_t status = r.run(&data);
    CHECK_EQ(status & 0x10, 0x10); // ends with Record Not Found after sector 10
    CHECK_EQ(status & 0x04, 0);    // no lost data
    CHECK_EQ(data.size(), size_t{2560});
    CHECK_EQ(data[0], 1);
    CHECK_EQ(data[2559], 10);
}

TEST_CASE("wrong density finds nothing") {
    Rig r(false);
    r.fdc.write(0, kDrive0 | kMotor | kIntEnable | kDrqEnable | kMfm);
    r.setSector(1);
    std::vector<uint8_t> data;
    r.fdc.write(2, 0x88);
    CHECK_EQ(r.run(&data) & 0x10, 0x10);
    CHECK_EQ(data.size(), size_t{0});
}

TEST_CASE("write sector then read it back") {
    Rig r;
    r.fdc.write(3, 3);
    r.fdc.write(2, 0x13);
    r.run(nullptr);
    r.setTrack(3);
    r.setSector(7);
    std::vector<uint8_t> out(256);
    for (int i = 0; i < 256; ++i) out[i] = static_cast<uint8_t>(255 - i);
    r.fdc.write(2, 0xA8);
    CHECK_EQ(r.run(nullptr, &out) & 0x5C, 0);
    std::vector<uint8_t> in;
    r.fdc.write(2, 0x88);
    CHECK_EQ(r.run(&in) & 0x1C, 0);
    CHECK_EQ(in == out, true);
}

TEST_CASE("write protected disk refuses writes") {
    H37 fdc;
    fdc.disk(0).loadRaw(rawImage(), 40, 1, 10, 256, false, true);
    fdc.write(0, kDrive0 | kMotor | kIntEnable);
    fdc.write(1, 1);
    fdc.write(2, 1); // sector 1
    fdc.write(1, 0);
    fdc.write(2, 0xA8);
    uint8_t status = 0xFF;
    for (int i = 0; i < 10000 && !fdc.irq(); ++i) fdc.tick(100);
    status = fdc.read(2);
    CHECK_EQ(status & 0x40, 0x40);
}

TEST_CASE("read address returns the next ID") {
    Rig r;
    r.fdc.write(3, 4);
    r.fdc.write(2, 0x10);
    r.run(nullptr);
    std::vector<uint8_t> id;
    r.fdc.write(2, 0xC0);
    r.run(&id);
    CHECK_EQ(id.size(), size_t{6});
    CHECK_EQ(id[0], 4); // cylinder
    CHECK_EQ(id[3], 1); // 256 bytes
}

TEST_CASE("write track formats a new geometry") {
    Rig r(false);
    r.fdc.write(0, kDrive0 | kMotor | kIntEnable | kDrqEnable | kMfm);
    // A minimal MFM track: 16 sectors of 256 bytes.
    std::vector<uint8_t> stream(80, 0x4E);
    for (int s = 1; s <= 16; ++s) {
        stream.insert(stream.end(), 12, 0x00);
        stream.insert(stream.end(), 3, 0xF5);
        for (uint8_t b : {uint8_t(0xFE), uint8_t(0), uint8_t(0), uint8_t(s), uint8_t(1), uint8_t(0xF7)})
            stream.push_back(b);
        stream.insert(stream.end(), 22, 0x4E);
        stream.insert(stream.end(), 12, 0x00);
        stream.insert(stream.end(), 3, 0xF5);
        stream.push_back(0xFB);
        stream.insert(stream.end(), 256, 0xE5);
        stream.push_back(0xF7);
        stream.insert(stream.end(), 24, 0x4E);
    }
    r.fdc.write(2, 0xF0);
    CHECK_EQ(r.run(nullptr, &stream) & 0x44, 0);
    const SoftDisk::Track* t = r.fdc.disk(0).track(0, 0);
    CHECK_EQ(t->mfm, true);
    CHECK_EQ(t->sectors.size(), size_t{16});
    CHECK_EQ(t->sectors[15].number, 16);
    CHECK_EQ(t->sectors[15].data.size(), size_t{256});
    CHECK_EQ(t->sectors[15].data[0], 0xE5);
}

TEST_CASE("force interrupt stops a command") {
    Rig r;
    r.setSector(99); // never found
    r.fdc.write(2, 0x88);
    r.fdc.tick(10000);
    CHECK_EQ(r.fdc.busy(), true);
    r.fdc.write(2, 0xD0);
    CHECK_EQ(r.fdc.busy(), false);
    CHECK_EQ(r.fdc.irq(), false);
    r.fdc.write(2, 0xD8);
    CHECK_EQ(r.fdc.irq(), true);
}

int main() { return check::runAll(); }
