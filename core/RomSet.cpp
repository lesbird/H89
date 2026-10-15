#include "RomSet.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>
#include <vector>

namespace h89 {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

} // namespace

RomScan scanRomDirectory(const std::string& dir, bool preferMtr89) {
    RomScan scan;
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string ext = lower(entry.path().extension().string());
        if (ext == ".bin" || ext == ".rom") files.push_back(entry.path());
    }
    if (ec) {
        scan.error = "cannot read ROM folder " + dir;
        return scan;
    }
    std::sort(files.begin(), files.end());

    auto find = [&](const char* partNumber) -> fs::path {
        for (const auto& f : files)
            if (lower(f.filename().string()).find(partNumber) != std::string::npos) return f;
        return {};
    };
    auto load = [&](const fs::path& p, std::vector<uint8_t>& into, const char* what) {
        if (p.empty()) return false;
        into = loadFile(p.string());
        if (into.empty()) return false;
        scan.summary += std::string(what) + ": " + p.filename().string() + "\n";
        return true;
    };

    fs::path mtr90 = find("444-142"), mtr89 = find("444-62");
    bool use89 = mtr90.empty() || (preferMtr89 && !mtr89.empty());
    fs::path monitor = use89 ? mtr89 : mtr90;
    const char* monitorName = use89 ? "Monitor (MTR-89)" : "Monitor (MTR-90)";

    std::vector<std::string> missing;
    if (!load(monitor, scan.config.monitorRom, monitorName))
        missing.push_back("monitor ROM (444-142 MTR-90 or 444-62 MTR-89)");
    load(find("444-19"), scan.config.h17Rom, "H17 boot ROM");
    if (!load(find("444-46"), scan.config.terminal.rom, "H19 firmware"))
        missing.push_back("H19 firmware (444-46)");
    if (!load(find("444-29"), scan.config.terminal.font, "H19 font"))
        missing.push_back("H19 font (444-29)");

    if (!missing.empty()) {
        scan.error = "Missing from " + dir + ":";
        for (const auto& m : missing) scan.error += "\n  " + m;
    }
    return scan;
}

} // namespace h89
