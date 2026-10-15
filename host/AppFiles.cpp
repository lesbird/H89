#include "AppFiles.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace h89 {

namespace fs = std::filesystem;

std::string executableDir() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    return fs::path(buf).parent_path().string();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    std::error_code ec;
    fs::path p = fs::canonical(buf, ec);
    return (ec ? fs::path(buf) : p).parent_path().string();
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.parent_path().string();
#endif
}

std::string findBundledDir(const std::string& name) {
    std::error_code ec;
    std::string exe = executableDir();
    fs::path dir = exe;
    for (int up = 0; !exe.empty() && up <= 3; ++up) {
        if (fs::is_directory(dir / name, ec)) return (dir / name).string();
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    if (fs::is_directory(name, ec)) return fs::absolute(name, ec).string();
    return {};
}

static std::string homeDir() {
#if defined(_WIN32)
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return home ? home : ".";
}

std::string userDocumentsDir() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, buf))) return buf;
#endif
    return homeDir();
}

std::string userDataDir() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) return (fs::path(buf) / "H89").string();
    return (fs::path(homeDir()) / "H89").string();
#elif defined(__APPLE__)
    return (fs::path(homeDir()) / "Library" / "Application Support" / "H89").string();
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path base = xdg && *xdg ? fs::path(xdg) : fs::path(homeDir()) / ".config";
    return (base / "h89").string();
#endif
}

std::string userDisksDir() {
#if defined(_WIN32)
    return (fs::path(userDocumentsDir()) / "H89" / "Disks").string();
#else
    return (fs::path(userDataDir()) / "Disks").string();
#endif
}

std::vector<std::pair<int, std::string>> starterDisks(const std::string& dir) {
    auto upper = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    };
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec))
        if (entry.is_regular_file(ec)) files.push_back(entry.path());
    std::sort(files.begin(), files.end());

    const struct {
        int drive;
        const char* system;
        const char* extension;
    } wanted[] = {{0, "HDOS", ".H8D"}, {1, "CPM", ".H8D"}, {3, "CPM", ".H37"}, {4, "HDOS", ".H37"}};
    std::vector<std::pair<int, std::string>> picks;
    for (const auto& w : wanted) {
        for (const auto& f : files) {
            if (upper(f.extension().string()) == w.extension &&
                upper(f.filename().string()).find(w.system) != std::string::npos) {
                picks.emplace_back(w.drive, f.string());
                break;
            }
        }
    }
    return picks;
}

std::vector<std::string> copyBundledFiles(const std::string& from, const std::string& to, bool overwrite,
                                          std::string* error) {
    std::vector<std::string> copied;
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        if (error) *error = "cannot create " + to + ": " + ec.message();
        return copied;
    }
    for (const auto& entry : fs::directory_iterator(from, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        fs::path dest = fs::path(to) / name;
        if (!overwrite && fs::exists(dest, ec)) continue;
        auto options = overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::skip_existing;
        if (fs::copy_file(entry.path(), dest, options, ec)) {
            // Bundles are often read-only; the copies are the user's to write.
            fs::permissions(dest, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::add, ec);
            copied.push_back(name);
        } else if (ec && error) {
            *error = "cannot copy " + name + ": " + ec.message();
        }
    }
    if (ec && error && error->empty()) *error = "cannot read " + from + ": " + ec.message();
    return copied;
}

// --- Settings ------------------------------------------------------------------

bool Settings::load() {
    std::ifstream in(path_);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        values_[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return true;
}

bool Settings::save() const {
    std::error_code ec;
    fs::create_directories(fs::path(path_).parent_path(), ec);
    std::ofstream out(path_, std::ios::trunc);
    for (const auto& [key, value] : values_) out << key << '=' << value << '\n';
    return static_cast<bool>(out);
}

std::string Settings::get(const std::string& key, const std::string& fallback) const {
    auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

int Settings::getInt(const std::string& key, int fallback) const {
    auto it = values_.find(key);
    if (it == values_.end() || it->second.empty()) return fallback;
    char* end = nullptr;
    long v = std::strtol(it->second.c_str(), &end, 10);
    return *end ? fallback : static_cast<int>(v);
}

bool Settings::getBool(const std::string& key, bool fallback) const {
    return getInt(key, fallback ? 1 : 0) != 0;
}

void Settings::set(const std::string& key, const std::string& value) {
    std::string v = value;
    for (char& c : v)
        if (c == '\n' || c == '\r') c = ' ';
    values_[key] = v;
}

// --- Sounds ----------------------------------------------------------------------

namespace {

constexpr int kSampleRate = 44100;

std::vector<uint8_t> wav(const std::vector<int16_t>& samples) {
    std::vector<uint8_t> out;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto u16 = [&](uint16_t v) { out.push_back(v & 0xFF), out.push_back(v >> 8); };
    auto tag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
    uint32_t dataBytes = static_cast<uint32_t>(samples.size() * 2);
    tag("RIFF"), u32(36 + dataBytes), tag("WAVE");
    tag("fmt "), u32(16), u16(1), u16(1), u32(kSampleRate), u32(kSampleRate * 2), u16(2), u16(16);
    tag("data"), u32(dataBytes);
    for (int16_t s : samples) u16(static_cast<uint16_t>(s));
    return out;
}

// The H19's speaker is driven by a square wave (its 2.048 MHz clock divided
// by 1024) gated for a fixed time, through a small speaker that rounds the
// edges off. A one-pole low-pass stands in for the speaker.
std::vector<int16_t> tone(double seconds, double amplitude) {
    const double freq = 2'048'000.0 / 1024.0;
    size_t n = static_cast<size_t>(seconds * kSampleRate);
    std::vector<int16_t> s(n);
    double y = 0;
    const double alpha = 0.35;
    for (size_t i = 0; i < n; ++i) {
        double phase = std::fmod(i * freq / kSampleRate, 1.0);
        double x = phase < 0.5 ? 1.0 : -1.0;
        y += alpha * (x - y);
        // Short ramps at each end so the sound doesn't pop.
        double ramp = std::min({1.0, i / (0.002 * kSampleRate), (n - i) / (0.002 * kSampleRate)});
        s[i] = static_cast<int16_t>(y * ramp * amplitude * 32767);
    }
    return s;
}

} // namespace

std::vector<uint8_t> makeBellSound() { return wav(tone(0.1, 0.25)); }
std::vector<uint8_t> makeKeyClickSound() { return wav(tone(0.004, 0.2)); }

} // namespace h89
