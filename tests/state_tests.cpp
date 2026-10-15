// Save states: a machine restored from a snapshot must carry on exactly as
// the original does. Needs the ROMs (argument 1) and, optionally, a folder
// of disk images (argument 2) to exercise the floppy controllers.

#include "Machine.h"
#include "RomSet.h"
#include "check.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

h89::MachineConfig gConfig;
std::string gDisks;

std::string firstImage(const char* fragment, const char* ext) {
    if (gDisks.empty()) return "";
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(gDisks, ec)) {
        std::string name = e.path().filename().string(), lower;
        for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.find(fragment) != std::string::npos && e.path().extension().string().size() == 4 &&
            lower.substr(lower.size() - 4) == ext)
            return e.path().string();
    }
    return "";
}

void runSeconds(h89::Machine& m, double seconds) {
    for (int i = 0; i < seconds * 1000; ++i) m.run(h89::Machine::kCpuHz / 1000);
}

void typeLine(h89::Machine& m, const std::string& line) {
    for (char c : line) {
        m.terminal()->typeChar(static_cast<uint8_t>(c));
        while (!m.terminal()->keyboardIdle()) m.run(2048);
    }
}

// Runs `a` and a copy restored from its snapshot side by side; their
// snapshots must stay identical.
void checkContinuation(h89::Machine& a, double seconds) {
    std::vector<uint8_t> snapshot = a.saveState();
    h89::Machine b(gConfig);
    std::string error;
    std::vector<std::string> warnings;
    CHECK_EQ(b.loadState(snapshot, &error, &warnings), true);
    CHECK_EQ(error, std::string(""));
    CHECK_EQ(warnings.size(), size_t{0});
    CHECK_EQ(b.saveState() == snapshot, true);
    runSeconds(a, seconds);
    runSeconds(b, seconds);
    CHECK_EQ(a.saveState() == b.saveState(), true);
    CHECK_EQ(a.terminal()->screenText(), b.terminal()->screenText());
}

} // namespace

TEST_CASE("restored machine continues identically at the monitor") {
    h89::Machine a(gConfig);
    runSeconds(a, 1.5);
    typeLine(a, "S0000\r"); // mid-command, with the timer and keyboard busy
    checkContinuation(a, 2);
}

TEST_CASE("restored machine continues identically while booting from the H17") {
    std::string disk = firstImage("hdos", ".h8d");
    if (disk.empty()) return;
    h89::Machine a(gConfig);
    std::string error;
    CHECK_EQ(a.h17()->insertDisk(0, disk, true, &error), true);
    runSeconds(a, 1.5);
    typeLine(a, "B\r");
    runSeconds(a, 0.7); // in the middle of reading the disk
    checkContinuation(a, 5);
}

TEST_CASE("restored machine continues identically while booting from the H37") {
    std::string disk = firstImage("cpm", ".h37");
    if (disk.empty()) return;
    gConfig.sw501 |= h89::sw501::kBootFrom170;
    h89::Machine a(gConfig);
    std::string error;
    CHECK_EQ(a.h37()->insertDisk(0, disk, true, &error), true);
    runSeconds(a, 1.5);
    typeLine(a, "B\r");
    runSeconds(a, 1.6); // mid-transfer
    checkContinuation(a, 5);
    gConfig.sw501 &= ~h89::sw501::kBootFrom170;
}

TEST_CASE("state from different ROMs is refused and leaves the machine alone") {
    h89::Machine a(gConfig);
    runSeconds(a, 0.5);
    std::vector<uint8_t> snapshot = a.saveState();
    h89::MachineConfig other = gConfig;
    other.monitorRom[100] ^= 0xFF;
    h89::Machine b(other);
    runSeconds(b, 0.2);
    std::vector<uint8_t> before = b.saveState();
    std::string error;
    CHECK_EQ(b.loadState(snapshot, &error), false);
    CHECK_EQ(error.empty(), false);
    CHECK_EQ(b.saveState() == before, true);

    std::vector<uint8_t> truncated(snapshot.begin(), snapshot.begin() + snapshot.size() / 2);
    CHECK_EQ(a.loadState(truncated, &error), false);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <rom-folder> [disk-folder]\n", argv[0]);
        return 2;
    }
    h89::RomScan scan = h89::scanRomDirectory(argv[1]);
    if (!scan.ok()) {
        std::fprintf(stderr, "%s\n", scan.error.c_str());
        return 2;
    }
    gConfig = scan.config;
    if (argc > 2) gDisks = argv[2];
    return check::runAll();
}
