#include "check.h"
#include "AppFiles.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// A fresh, empty folder under the system temp folder.
fs::path scratch(const char* name) {
    fs::path dir = fs::temp_directory_path() / "h89_appfiles_tests" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write(const fs::path& p, const std::string& text) {
    std::ofstream(p, std::ios::binary) << text;
}

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

} // namespace

TEST_CASE("settings round trip") {
    fs::path file = scratch("settings") / "sub" / "settings.ini";
    h89::Settings s(file.string());
    CHECK_EQ(s.load(), false);
    s.set("roms", "C:\\H89\\roms");
    s.setInt("memory", 48);
    s.setBool("crt", false);
    s.set("odd", "line\nbreak");
    CHECK_EQ(s.save(), true);

    h89::Settings t(file.string());
    CHECK_EQ(t.load(), true);
    CHECK_EQ(t.get("roms"), std::string("C:\\H89\\roms"));
    CHECK_EQ(t.getInt("memory", 64), 48);
    CHECK_EQ(t.getBool("crt", true), false);
    CHECK_EQ(t.get("odd"), std::string("line break"));
    CHECK_EQ(t.getInt("missing", 7), 7);
    CHECK_EQ(t.getInt("roms", 7), 7); // not a number
    t.remove("roms");
    CHECK_EQ(t.has("roms"), false);
}

TEST_CASE("bundled files are copied once, then restored on request") {
    fs::path from = scratch("bundle"), to = scratch("user") / "Disks";
    write(from / "HDOS.h8d", "original");
    write(from / ".DS_Store", "junk");
    auto copied = h89::copyBundledFiles(from.string(), to.string());
    CHECK_EQ(copied.size(), size_t{1});
    CHECK_EQ(read(to / "HDOS.h8d"), std::string("original"));
    CHECK_EQ(fs::exists(to / ".DS_Store"), false);

    write(to / "HDOS.h8d", "changed by the user");
    CHECK_EQ(h89::copyBundledFiles(from.string(), to.string()).size(), size_t{0});
    CHECK_EQ(read(to / "HDOS.h8d"), std::string("changed by the user"));

    CHECK_EQ(h89::copyBundledFiles(from.string(), to.string(), true).size(), size_t{1});
    CHECK_EQ(read(to / "HDOS.h8d"), std::string("original"));
}

TEST_CASE("starter disks go in SY0:, SY1:, DK0: and DK1:") {
    fs::path dir = scratch("starter");
    for (const char* name : {"CPM_2-2-03.H8D", "HDOS_2-0_System.h8d", "CPM-BOOT.H37", "HDOS-2.H37", "notes.txt"})
        write(dir / name, "x");
    auto picks = h89::starterDisks(dir.string());
    CHECK_EQ(picks.size(), size_t{4});
    if (picks.size() != 4) return;
    CHECK_EQ(picks[0].first, 0);
    CHECK_EQ(fs::path(picks[0].second).filename().string(), std::string("HDOS_2-0_System.h8d"));
    CHECK_EQ(picks[1].first, 1);
    CHECK_EQ(fs::path(picks[1].second).filename().string(), std::string("CPM_2-2-03.H8D"));
    CHECK_EQ(picks[2].first, 3);
    CHECK_EQ(fs::path(picks[2].second).filename().string(), std::string("CPM-BOOT.H37"));
    CHECK_EQ(picks[3].first, 4);
    CHECK_EQ(fs::path(picks[3].second).filename().string(), std::string("HDOS-2.H37"));
}

TEST_CASE("speaker sounds are valid WAV files") {
    for (const auto& wav : {h89::makeBellSound(), h89::makeKeyClickSound()}) {
        CHECK_EQ(wav.size() > 44, true);
        CHECK_EQ(std::string(wav.begin(), wav.begin() + 4), std::string("RIFF"));
        CHECK_EQ(std::string(wav.begin() + 8, wav.begin() + 12), std::string("WAVE"));
        uint32_t riffSize = wav[4] | wav[5] << 8 | wav[6] << 16 | static_cast<uint32_t>(wav[7]) << 24;
        CHECK_EQ(riffSize, static_cast<uint32_t>(wav.size() - 8));
    }
}

int main() { return check::runAll(); }
