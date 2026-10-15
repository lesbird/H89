#include "SoftDisk.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace h89 {

namespace {

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

constexpr size_t kTrailerSize = 32;

// "SPT=10 SSZ=0256 TRK=40 SID=1 FM"
bool parseTrailer(const uint8_t* t, int* spt, int* ssz, int* trk, int* sid, bool* mfm) {
    std::string s(reinterpret_cast<const char*>(t), kTrailerSize);
    s = s.substr(0, s.find('\0'));
    if (std::sscanf(s.c_str(), "SPT=%d SSZ=%d TRK=%d SID=%d", spt, ssz, trk, sid) != 4) return false;
    *mfm = s.find(" MFM") != std::string::npos;
    return *spt > 0 && *spt <= 32 && *ssz >= 128 && *ssz <= 1024 && *trk > 0 && *trk <= 86 &&
           (*sid == 1 || *sid == 2);
}

uint8_t sizeCodeFor(size_t size) {
    uint8_t code = 0;
    while ((128u << code) < size && code < 6) ++code;
    return code;
}

} // namespace

bool SoftDisk::load(const std::string& path, bool readOnly, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        setError(error, "cannot open " + path);
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    bool ok;
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "IMD ", 4) == 0) {
        ok = loadImd(bytes, readOnly, error);
    } else {
        int spt, ssz, trk, sid;
        bool mfm;
        bool trailer = bytes.size() > kTrailerSize &&
                       parseTrailer(&bytes[bytes.size() - kTrailerSize], &spt, &ssz, &trk, &sid, &mfm);
        if (trailer) {
            bytes.resize(bytes.size() - kTrailerSize);
        } else {
            // Common H37 geometries by size.
            struct Guess {
                size_t size;
                int trk, sid, spt, ssz;
                bool mfm;
            } guesses[] = {
                {102400, 40, 1, 10, 256, false}, {163840, 40, 1, 16, 256, true},
                {327680, 40, 2, 16, 256, true},  {655360, 80, 2, 16, 256, true},
            };
            auto g = std::find_if(std::begin(guesses), std::end(guesses),
                                  [&](const Guess& g) { return g.size == bytes.size(); });
            if (g == std::end(guesses)) {
                setError(error, path + " is not a disk image this emulator recognises (use an ImageDisk .imd file).");
                return false;
            }
            trk = g->trk, sid = g->sid, spt = g->spt, ssz = g->ssz, mfm = g->mfm;
        }
        if (bytes.size() != static_cast<size_t>(trk) * sid * spt * ssz) {
            setError(error, path + ": size doesn't match its geometry");
            return false;
        }
        ok = loadRaw(std::move(bytes), trk, sid, spt, ssz, mfm, readOnly);
        rawTrailer_ = trailer;
    }
    if (ok) path_ = path;
    return ok;
}

bool SoftDisk::loadRaw(std::vector<uint8_t> bytes, int cylinders, int sides, int sectors, int sectorSize, bool mfm,
                       bool readOnly) {
    if (bytes.size() != static_cast<size_t>(cylinders) * sides * sectors * sectorSize) return false;
    tracks_.assign(static_cast<size_t>(cylinders) * sides, Track{});
    cylinders_ = cylinders;
    sides_ = sides;
    size_t offset = 0;
    for (int c = 0; c < cylinders; ++c) {
        for (int h = 0; h < sides; ++h) {
            Track& t = tracks_[c * sides + h];
            t.mfm = mfm;
            for (int s = 0; s < sectors; ++s) {
                Sector sec;
                sec.cylinder = static_cast<uint8_t>(c);
                sec.head = static_cast<uint8_t>(h);
                sec.number = static_cast<uint8_t>(s + 1);
                sec.sizeCode = sizeCodeFor(sectorSize);
                sec.data.assign(bytes.begin() + offset, bytes.begin() + offset + sectorSize);
                offset += sectorSize;
                t.sectors.push_back(std::move(sec));
            }
        }
    }
    readOnly_ = readOnly;
    imd_ = false;
    path_.clear();
    rawGeometry_ = {sectors, sectorSize, mfm};
    rawTrailer_ = false;
    return true;
}

bool SoftDisk::loadImd(const std::vector<uint8_t>& bytes, bool readOnly, std::string* error) {
    auto fail = [&](const char* why) {
        setError(error, std::string("Bad ImageDisk file: ") + why);
        return false;
    };
    auto eof = std::find(bytes.begin(), bytes.end(), 0x1A);
    if (eof == bytes.end()) return fail("no end of comment");
    imdHeader_.assign(bytes.begin(), eof);
    size_t p = static_cast<size_t>(eof - bytes.begin()) + 1;

    std::vector<std::pair<int, Track>> found; // index cylinder * 2 + head
    int maxCylinder = -1, maxHead = 0;
    while (p < bytes.size()) {
        if (p + 5 > bytes.size()) return fail("truncated track header");
        uint8_t mode = bytes[p], cylinder = bytes[p + 1], headByte = bytes[p + 2], count = bytes[p + 3],
                sizeCode = bytes[p + 4];
        p += 5;
        if (mode > 5) return fail("unknown recording mode");
        if (sizeCode > 6) return fail("variable sector sizes are not supported");
        int head = headByte & 0x0F;
        if (head > 1) return fail("more than two sides");
        size_t size = 128u << sizeCode;
        auto take = [&](size_t n) -> const uint8_t* {
            if (p + n > bytes.size()) return nullptr;
            const uint8_t* q = &bytes[p];
            p += n;
            return q;
        };
        const uint8_t* numbers = take(count);
        const uint8_t* cylinders = (headByte & 0x80) ? take(count) : nullptr;
        const uint8_t* heads = (headByte & 0x40) ? take(count) : nullptr;
        if (!numbers || ((headByte & 0x80) && !cylinders) || ((headByte & 0x40) && !heads))
            return fail("truncated sector map");
        Track t;
        t.mfm = mode >= 3;
        for (int i = 0; i < count; ++i) {
            Sector s;
            s.cylinder = cylinders ? cylinders[i] : cylinder;
            s.head = heads ? heads[i] : static_cast<uint8_t>(head);
            s.number = numbers[i];
            s.sizeCode = sizeCode;
            const uint8_t* type = take(1);
            if (!type || *type > 8) return fail("bad sector record");
            if (*type != 0) {
                bool compressed = *type % 2 == 0;
                s.deleted = *type == 3 || *type == 4 || *type >= 7;
                s.crcError = *type >= 5;
                const uint8_t* d = take(compressed ? 1 : size);
                if (!d) return fail("truncated sector data");
                if (compressed) s.data.assign(size, *d);
                else s.data.assign(d, d + size);
            }
            t.sectors.push_back(std::move(s));
        }
        maxCylinder = std::max<int>(maxCylinder, cylinder);
        maxHead = std::max(maxHead, head);
        found.emplace_back(cylinder * 2 + head, std::move(t));
    }
    if (maxCylinder < 0) return fail("no tracks");
    cylinders_ = maxCylinder + 1;
    sides_ = maxHead + 1;
    tracks_.assign(static_cast<size_t>(cylinders_) * sides_, Track{});
    for (auto& [index, t] : found) tracks_[(index / 2) * sides_ + index % 2] = std::move(t);
    readOnly_ = readOnly;
    imd_ = true;
    path_.clear();
    return true;
}

const SoftDisk::Track* SoftDisk::track(int cylinder, int head) const {
    if (cylinder < 0 || cylinder >= cylinders_ || head < 0 || head >= sides_) return nullptr;
    return &tracks_[cylinder * sides_ + head];
}

SoftDisk::Track* SoftDisk::mutableTrack(int cylinder, int head) {
    return const_cast<Track*>(track(cylinder, head));
}

bool SoftDisk::writeSector(int cylinder, int head, int index, const std::vector<uint8_t>& data, bool deleted) {
    Track* t = mutableTrack(cylinder, head);
    if (readOnly_ || !t || index < 0 || index >= static_cast<int>(t->sectors.size())) return false;
    Sector& s = t->sectors[index];
    s.data = data;
    s.deleted = deleted;
    s.crcError = false;
    return save();
}

bool SoftDisk::formatTrack(int cylinder, int head, Track track) {
    if (readOnly_ || cylinder < 0 || cylinder >= 86 || head < 0 || head > 1) return false;
    // Formatting can extend the disk (more cylinders or a second side).
    if (cylinder >= cylinders_ || head >= sides_) {
        int newCylinders = std::max(cylinders_, cylinder + 1), newSides = std::max(sides_, head + 1);
        std::vector<Track> grown(static_cast<size_t>(newCylinders) * newSides);
        for (int c = 0; c < cylinders_; ++c)
            for (int h = 0; h < sides_; ++h) grown[c * newSides + h] = std::move(tracks_[c * sides_ + h]);
        tracks_ = std::move(grown);
        cylinders_ = newCylinders;
        sides_ = newSides;
    }
    tracks_[cylinder * sides_ + head] = std::move(track);
    return save();
}

bool SoftDisk::rawBytes(std::vector<uint8_t>* out) const {
    out->clear();
    for (const Track& t : tracks_) {
        if (t.mfm != rawGeometry_.mfm || static_cast<int>(t.sectors.size()) != rawGeometry_.sectors) return false;
        for (int n = 1; n <= rawGeometry_.sectors; ++n) {
            auto s = std::find_if(t.sectors.begin(), t.sectors.end(), [&](const Sector& s) { return s.number == n; });
            if (s == t.sectors.end() || static_cast<int>(s->data.size()) != rawGeometry_.sectorSize) return false;
            out->insert(out->end(), s->data.begin(), s->data.end());
        }
    }
    return true;
}

std::vector<uint8_t> SoftDisk::imdBytes() const {
    std::vector<uint8_t> out(imdHeader_.begin(), imdHeader_.end());
    if (imdHeader_.empty()) {
        const char* header = "IMD 1.18: written by the H89 emulator\r\n";
        out.assign(header, header + std::strlen(header));
    }
    out.push_back(0x1A);
    for (int c = 0; c < cylinders_; ++c) {
        for (int h = 0; h < sides_; ++h) {
            const Track& t = tracks_[c * sides_ + h];
            if (t.sectors.empty()) continue;
            uint8_t sizeCode = t.sectors[0].sizeCode;
            size_t size = 128u << std::min<uint8_t>(sizeCode, 6);
            bool cylMap = false, headMap = false;
            for (const Sector& s : t.sectors) {
                cylMap = cylMap || s.cylinder != c;
                headMap = headMap || s.head != h;
            }
            out.push_back(t.mfm ? 5 : 2); // 250 kbit/s MFM / FM (5.25" double density drive)
            out.push_back(static_cast<uint8_t>(c));
            out.push_back(static_cast<uint8_t>(h | (cylMap ? 0x80 : 0) | (headMap ? 0x40 : 0)));
            out.push_back(static_cast<uint8_t>(t.sectors.size()));
            out.push_back(sizeCode);
            for (const Sector& s : t.sectors) out.push_back(s.number);
            if (cylMap)
                for (const Sector& s : t.sectors) out.push_back(s.cylinder);
            if (headMap)
                for (const Sector& s : t.sectors) out.push_back(s.head);
            for (const Sector& s : t.sectors) {
                if (s.data.empty()) {
                    out.push_back(0);
                    continue;
                }
                std::vector<uint8_t> d = s.data;
                d.resize(size, 0);
                bool same = std::all_of(d.begin(), d.end(), [&](uint8_t b) { return b == d[0]; });
                uint8_t type = 1 + (same ? 1 : 0) + (s.deleted ? 2 : 0) + (s.crcError ? 4 : 0);
                out.push_back(type);
                if (same) out.push_back(d[0]);
                else out.insert(out.end(), d.begin(), d.end());
            }
        }
    }
    return out;
}

bool SoftDisk::save() {
    if (path_.empty()) return true;
    std::vector<uint8_t> bytes;
    if (imd_) {
        bytes = imdBytes();
    } else if (!rawBytes(&bytes)) {
        // Reformatted to another geometry: adopt it once every track has it
        // (the trailer records it); until then the change stays in memory.
        const Track& first = tracks_[0];
        if (first.sectors.empty()) return true;
        Geometry old = rawGeometry_;
        rawGeometry_ = {static_cast<int>(first.sectors.size()), static_cast<int>(first.sectors[0].data.size()),
                        first.mfm};
        if (!rawBytes(&bytes)) {
            rawGeometry_ = old;
            return true;
        }
        rawTrailer_ = true;
    }
    if (!imd_ && rawTrailer_) {
        char trailer[kTrailerSize + 1] = {}; // the trailer itself needs no terminating NUL
        std::snprintf(trailer, sizeof trailer, "SPT=%d SSZ=%04d TRK=%d SID=%d %s", rawGeometry_.sectors,
                      rawGeometry_.sectorSize, cylinders_, sides_, rawGeometry_.mfm ? "MFM" : "FM");
        bytes.insert(bytes.end(), trailer, trailer + kTrailerSize);
    }
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

} // namespace h89
