// The H19 as a terminal on a host serial port, driven through a
// pseudo-terminal standing in for a USB serial adapter (POSIX only).

#include "check.h"
#include "RomSet.h"
#include "TerminalLink.h"
#include "terminal/H19.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace {

h89::H19Config gConfig;

struct Pty {
    int master = -1, slave = -1;
    std::string name;
    Pty() {
        char path[128];
        if (openpty(&master, &slave, path, nullptr, nullptr) == 0) {
            name = path;
            fcntl(master, F_SETFL, O_NONBLOCK);
        }
    }
    ~Pty() {
        if (master >= 0) close(master);
        if (slave >= 0) close(slave);
    }
};

bool screenHas(const h89::H19& term, const std::string& text) {
    return term.screenText().find(text) != std::string::npos;
}

} // namespace

TEST_CASE("a fast burst reaches the screen without losing characters") {
    Pty pty;
    CHECK_EQ(pty.name.empty(), false);
    h89::H19 term(gConfig);
    h89::TerminalLink link(term);
    h89::SerialSettings settings;
    CHECK_EQ(h89::SerialSettings::parse(pty.name + ":115200:8N1", settings), true);
    std::string error;
    CHECK_EQ(link.open(settings, &error), true);
    for (int f = 0; f < 60; ++f) link.runFrame(); // the firmware starts up

    // 2000 lines in one go: far more than a real H19 could take at 115200.
    std::string burst = "\x1b" "E";
    char line[32];
    for (int i = 0; i < 2000; ++i) {
        std::snprintf(line, sizeof line, "LINE %04d ABCDEFGHIJ\r\n", i);
        burst += line;
    }
    burst += "DONE";
    size_t sent = 0;
    int f = 0;
    for (; f < 3000 && !screenHas(term, "DONE"); ++f) {
        if (sent < burst.size()) {
            ssize_t n = write(pty.master, burst.data() + sent, burst.size() - sent);
            if (n > 0) sent += static_cast<size_t>(n);
        }
        link.runFrame();
        char back[64];
        if (read(pty.master, back, sizeof back) > 0) std::printf("  the H19's flow control reached the port\n");
    }
    if (!screenHas(term, "DONE"))
        std::printf("  sent %zu of %zu, received %zu\n%s\n", sent, burst.size(), link.received().size(),
                    term.screenText().c_str());
    CHECK_EQ(screenHas(term, "DONE"), true);
    std::printf("  %zu bytes took %d frames (%.2f s at 60 Hz)\n", burst.size(), f, f / 60.0);
    // The last screenful is intact, line after line.
    for (int i = 1977; i < 2000; ++i) {
        std::snprintf(line, sizeof line, "LINE %04d ABCDEFGHIJ", i);
        if (!screenHas(term, line)) std::printf("  missing: %s\n", line);
        CHECK_EQ(screenHas(term, line), true);
    }
}

TEST_CASE("typing goes out of the port") {
    Pty pty;
    h89::H19 term(gConfig);
    h89::TerminalLink link(term);
    h89::SerialSettings settings;
    h89::SerialSettings::parse(pty.name + ":38400", settings);
    CHECK_EQ(link.open(settings), true);
    for (int f = 0; f < 60; ++f) link.runFrame();
    for (char c : std::string("DIR\r\x13")) term.typeChar(static_cast<uint8_t>(c));
    std::string got;
    for (int f = 0; f < 120 && got.size() < 5; ++f) {
        link.runFrame();
        char buf[64];
        ssize_t n = read(pty.master, buf, sizeof buf);
        if (n > 0) got.append(buf, static_cast<size_t>(n));
    }
    CHECK_EQ(got, std::string("DIR\r\x13")); // a typed Ctrl-S goes out
}

TEST_CASE("a port that goes away is noticed") {
    h89::H19 term(gConfig);
    h89::TerminalLink link(term);
    std::string name;
    {
        Pty pty;
        name = pty.name;
        h89::SerialSettings settings;
        h89::SerialSettings::parse(pty.name + ":9600", settings);
        CHECK_EQ(link.open(settings), true);
        link.runFrame();
        CHECK_EQ(link.connected(), true);
    } // both ends closed: the "adapter" is unplugged
    for (int f = 0; f < 5; ++f) link.runFrame();
    CHECK_EQ(link.connected(), false);
    CHECK_EQ(link.description().find("not connected") != std::string::npos, true);
}

TEST_CASE("a missing port is retried and the terminal still runs") {
    h89::H19 term(gConfig);
    h89::TerminalLink link(term);
    h89::SerialSettings settings;
    h89::SerialSettings::parse("/dev/no-such-adapter:9600", settings);
    std::string error;
    CHECK_EQ(link.open(settings, &error), false);
    CHECK_EQ(error.empty(), false);
    CHECK_EQ(link.description().find("not connected") != std::string::npos, true);
    for (int f = 0; f < 60; ++f) link.runFrame();
    term.setOffline(true); // local typing echoes on screen
    for (char c : std::string("HI")) term.typeChar(static_cast<uint8_t>(c));
    for (int f = 0; f < 60; ++f) link.runFrame();
    CHECK_EQ(screenHas(term, "HI"), true);
}

namespace {

// Runs a transfer on the terminal's side and one on the "computer" (the far
// end of the pty), until both finish.
void runPair(h89::TerminalLink& link, h89::FileTransfer& far, Pty& pty, const std::string& saveDir) {
    std::vector<uint8_t> toNear;
    for (int f = 0; f < 60 * 60; ++f) {
        h89::FileTransfer* near = link.transfer();
        if (near->state() == h89::FileTransfer::State::NeedsSavePath)
            near->setSavePath(saveDir + "/" + near->offeredName());
        if (far.state() == h89::FileTransfer::State::NeedsSavePath) far.setSavePath(saveDir + "/" + far.offeredName());
        if (near->finished() && far.finished()) break;
        link.runFrame();
        auto now = h89::FileTransfer::Clock::now();
        uint8_t buf[4096];
        ssize_t n;
        while ((n = read(pty.master, buf, sizeof buf)) > 0) far.received(buf, static_cast<size_t>(n), now);
        far.poll(now, toNear, 4096);
        // The pty takes about 1K at a time; keep the rest for next time.
        ssize_t w = toNear.empty() ? 0 : write(pty.master, toNear.data(), toNear.size());
        if (w > 0) toNear.erase(toNear.begin(), toNear.begin() + w);
        usleep(2000);
    }
}

std::vector<uint8_t> fileBytes(const std::string& path) {
    std::vector<uint8_t> v;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        int c;
        while ((c = std::fgetc(f)) != EOF) v.push_back(static_cast<uint8_t>(c));
        std::fclose(f);
    }
    return v;
}

} // namespace

TEST_CASE("files go both ways over the port") {
    Pty pty;
    h89::H19 term(gConfig);
    h89::TerminalLink link(term);
    h89::SerialSettings settings;
    h89::SerialSettings::parse(pty.name + ":230400", settings);
    CHECK_EQ(link.open(settings), true);
    for (int f = 0; f < 30; ++f) link.runFrame();

    std::string dir = "/tmp/h89_link_transfer";
    std::system(("rm -rf " + dir + " && mkdir -p " + dir + "/in " + dir + "/out").c_str());
    std::vector<uint8_t> data(30'000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 131 + (i >> 7));
    if (std::FILE* f = std::fopen((dir + "/in/prog.com").c_str(), "wb")) {
        std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
    }

    // Upload with ZMODEM: the "computer" receives.
    CHECK_EQ(link.startTransfer(h89::zmodemSend({dir + "/in/prog.com"})), true);
    auto farZ = h89::zmodemReceive();
    runPair(link, *farZ, pty, dir + "/out");
    CHECK_EQ(link.transfer()->state() == h89::FileTransfer::State::Done, true);
    CHECK_EQ(fileBytes(dir + "/out/PROG.COM") == data, true);
    link.clearTransfer();

    // Download with XMODEM: the "computer" sends.
    CHECK_EQ(link.startTransfer(h89::xmodemReceive(dir + "/out/down.com")), true);
    auto farX = h89::xmodemSend(dir + "/in/prog.com", true);
    runPair(link, *farX, pty, dir + "/out");
    CHECK_EQ(link.transfer()->state() == h89::FileTransfer::State::Done, true);
    if (link.transfer()->state() != h89::FileTransfer::State::Done)
        std::printf("  terminal: \"%s\", computer: \"%s\"\n", link.transfer()->status().c_str(),
                    farX->status().c_str());
    std::vector<uint8_t> got = fileBytes(dir + "/out/down.com");
    CHECK_EQ(got.size(), size_t{30'720}); // XMODEM-1K pads the last 304 bytes to a 1K block
    CHECK_EQ(got.size() >= data.size() && std::equal(data.begin(), data.end(), got.begin()), true);
    // Nothing of the protocols reached the screen.
    CHECK_EQ(screenHas(term, "*"), false);
    link.clearTransfer();
    CHECK_EQ(link.transfer() == nullptr, true);
}

TEST_CASE("serial specs parse") {
    h89::SerialSettings s;
    CHECK_EQ(h89::SerialSettings::parse("COM3", s), true);
    CHECK_EQ(s.device, std::string("COM3"));
    CHECK_EQ(s.baud, 9600);
    CHECK_EQ(h89::SerialSettings::parse("/dev/cu.usbserial-1410:460800:7E2:rtscts", s), true);
    CHECK_EQ(s.device, std::string("/dev/cu.usbserial-1410"));
    CHECK_EQ(s.baud, 460800);
    CHECK_EQ(s.format(), std::string("7E2"));
    CHECK_EQ(s.rtsCts, true);
    CHECK_EQ(s.spec(), std::string("/dev/cu.usbserial-1410:460800:7E2:rtscts"));
    CHECK_EQ(h89::SerialSettings::parse(":9600", s), false);
    CHECK_EQ(h89::SerialSettings::parse("COM3:5", s), false);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <rom-folder>\n", argv[0]);
        return 2;
    }
    h89::RomScan scan = h89::scanRomDirectory(argv[1]);
    if (!scan.ok()) {
        std::fprintf(stderr, "%s\n", scan.error.c_str());
        return 2;
    }
    gConfig = scan.config.terminal;
    return check::runAll();
}
