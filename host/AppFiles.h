#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace h89 {

// Where a front end finds its bundled files and keeps the user's.

// Folder holding the running executable ("" if unknown).
std::string executableDir();

// Finds a folder called `name` (such as "roms") next to the executable, or up
// to three levels above it so development builds find the source tree's
// copy, or else in the working folder. Returns "" if there is none.
std::string findBundledDir(const std::string& name);

// The user's documents folder (Windows) or home folder.
std::string userDocumentsDir();

// Where the user's copies of the bundled disks live: Documents\H89\Disks on
// Windows, the user data folder's Disks elsewhere.
std::string userDisksDir();

// Per-user application data: %APPDATA%\H89 on Windows, ~/Library/Application
// Support/H89 on macOS, $XDG_CONFIG_HOME/h89 (or ~/.config/h89) elsewhere.
std::string userDataDir();

// Copies each regular file in `from` that `to` doesn't have yet, creating
// `to` if needed; existing files are never overwritten. With `overwrite`,
// replaces them too (to restore originals). Returns the names copied.
std::vector<std::string> copyBundledFiles(const std::string& from, const std::string& to, bool overwrite = false,
                                          std::string* error = nullptr);

// Which of the disks in `dir` go in the drives the first time the emulator
// runs: HDOS and CP/M for the H17 in SY0: and SY1: (drives 0 and 1), and
// CP/M and HDOS for the H37 in DK0: and DK1: (drives 3 and 4). Pairs of
// (drive, path); drives without a matching image are left out.
std::vector<std::pair<int, std::string>> starterDisks(const std::string& dir);

// A flat key=value settings file, one setting per line.
class Settings {
public:
    explicit Settings(std::string path = {}) : path_(std::move(path)) {}

    bool load();
    bool save() const;
    const std::string& path() const { return path_; }

    std::string get(const std::string& key, const std::string& fallback = {}) const;
    int getInt(const std::string& key, int fallback) const;
    bool getBool(const std::string& key, bool fallback) const;
    bool has(const std::string& key) const { return values_.count(key) != 0; }
    void set(const std::string& key, const std::string& value);
    void setInt(const std::string& key, int value) { set(key, std::to_string(value)); }
    void setBool(const std::string& key, bool value) { set(key, value ? "1" : "0"); }
    void remove(const std::string& key) { values_.erase(key); }

private:
    std::string path_;
    std::map<std::string, std::string> values_;
};

// The H19's speaker sounds as WAV files (16-bit mono PCM) to hand to the
// host's sound player: the bell, and the click a key makes.
std::vector<uint8_t> makeBellSound();
std::vector<uint8_t> makeKeyClickSound();

} // namespace h89
