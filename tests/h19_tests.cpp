// H19 terminal tests against the real firmware: h19_tests <rom-folder>

#include "RomSet.h"
#include "check.h"
#include "terminal/H19.h"

#include <memory>
#include <string>

using h89::H19;
using h89::H19Key;

namespace {

h89::H19Config gConfig;

struct Terminal {
    H19 term{gConfig};
    std::string sent; // bytes the terminal transmitted to the computer

    Terminal() {
        term.serialOut = [this](uint8_t b) { sent += static_cast<char>(b); };
        run(0.5); // firmware initialisation
    }

    void run(double seconds) {
        long budget = static_cast<long>(seconds * H19::kCpuHz);
        while (budget > 0) budget -= term.step();
    }

    // Sends bytes from the "computer" and lets the terminal process them.
    void receive(const std::string& s) {
        for (char c : s) term.serialIn(static_cast<uint8_t>(c));
        run(0.05 + s.size() * 0.0015);
    }

    void type(const std::string& s) {
        for (char c : s) term.typeChar(static_cast<uint8_t>(c));
        run(0.05 + s.size() * 0.02);
    }

    std::string row(int r) {
        std::string text = term.screenText();
        size_t pos = 0;
        for (int i = 0; i < r && pos != std::string::npos; ++i) pos = text.find('\n', pos) + 1;
        return text.substr(pos, text.find('\n', pos) - pos);
    }
};

const std::string ESC = "\x1b";

} // namespace

TEST_CASE("Displays received text") {
    Terminal t;
    t.receive("Hello, H89\r\nSecond line");
    CHECK_EQ(t.row(0), std::string("Hello, H89"));
    CHECK_EQ(t.row(1), std::string("Second line"));
}

TEST_CASE("ESC E clears and ESC Y positions the cursor") {
    Terminal t;
    t.receive("junk" + ESC + "E");
    CHECK_EQ(t.row(0), std::string(""));
    // ESC Y row+32 col+32
    t.receive(ESC + "Y" + char(32 + 5) + char(32 + 10) + "X");
    CHECK_EQ(t.row(5), std::string(10, ' ') + "X");
}

TEST_CASE("25th line is enabled with ESC x 1") {
    Terminal t;
    CHECK_EQ(t.term.crtc().verticalDisplayed(), 24);
    t.receive(ESC + "x1" + ESC + "Y" + char(32 + 24) + char(32) + "STATUS LINE");
    t.run(0.1);
    CHECK_EQ(t.term.crtc().verticalDisplayed(), 25);
    CHECK_EQ(t.row(24), std::string("STATUS LINE"));
}

TEST_CASE("Typed characters are transmitted") {
    Terminal t;
    t.type("dir\r");
    CHECK_EQ(t.sent, std::string("dir\r"));
}

TEST_CASE("Control characters are transmitted") {
    Terminal t;
    t.type("\x03\x08\x1b\x7f");
    CHECK_EQ(t.sent, std::string("\x03\x08\x1b\x7f"));
}

TEST_CASE("Function keys send Heath escape sequences") {
    Terminal t;
    for (H19Key k : {H19Key::F1, H19Key::F5, H19Key::Erase, H19Key::Blue, H19Key::Red, H19Key::White}) {
        t.term.pressKey(k);
        t.run(0.05);
    }
    CHECK_EQ(t.sent, ESC + "S" + ESC + "W" + ESC + "J" + ESC + "P" + ESC + "Q" + ESC + "R");
}

TEST_CASE("Keypad sends digits, shifted keypad sends cursor keys") {
    Terminal t;
    t.term.pressKey(H19Key::Keypad7);
    t.run(0.05);
    t.term.pressKey(H19Key::Keypad8, true);
    t.run(0.05);
    t.term.pressKey(H19Key::Keypad2, true);
    t.run(0.05);
    t.term.pressKey(H19Key::KeypadEnter);
    t.run(0.05);
    CHECK_EQ(t.sent, "7" + ESC + "A" + ESC + "B" + "\r");
}

TEST_CASE("Lines scrolled off the top are kept for scrolling back") {
    Terminal t;
    for (int i = 0; i < 30; ++i) t.receive("line " + std::to_string(i) + "\r\n");
    // 30 lines and the cursor on a 31st: the first 7 have scrolled off.
    CHECK_EQ(t.term.scrollbackLines(), 7);
    CHECK_EQ(t.row(0), std::string("line 7"));
    t.term.setViewOffset(3);
    CHECK_EQ(t.row(0), std::string("line 4"));
    CHECK_EQ(t.row(3), std::string("line 7"));
    // New lines keep the view on the same text.
    t.receive("line 30\r\n");
    CHECK_EQ(t.term.viewOffset(), 4);
    CHECK_EQ(t.row(0), std::string("line 4"));
    t.term.setViewOffset(100); // clamps to the oldest line
    CHECK_EQ(t.term.viewOffset(), 8);
    CHECK_EQ(t.row(0), std::string("line 0"));
    // Typing returns to the live screen.
    t.type("x");
    CHECK_EQ(t.term.viewOffset(), 0);
    CHECK_EQ(t.row(0), std::string("line 8"));
}

TEST_CASE("The 25th line stays put while scrolled back") {
    Terminal t;
    t.receive(ESC + "x1" + ESC + "Y" + char(32 + 24) + char(32) + "STATUS" + ESC + "Y" + char(32 + 23) + char(32));
    for (int i = 0; i < 5; ++i) t.receive("line " + std::to_string(i) + "\r\n");
    CHECK_EQ(t.term.scrollbackLines(), 5);
    t.term.setViewOffset(5);
    CHECK_EQ(t.row(0), std::string(""));
    CHECK_EQ(t.row(24), std::string("STATUS"));
}

TEST_CASE("Scrollback keeps whole lines with the 25th line on") {
    // The firmware's 25th-line move overwrites the start of the top row just
    // before it scrolls off.
    Terminal t;
    t.receive(ESC + "x1" + ESC + "Y" + char(32 + 24) + char(32) + "STATUS" + ESC + "Y" + char(32 + 23) + char(32));
    const std::string text = "HCOS ON CP/M FOR H17 DISKS WITH OPTION(S) I, VERSION 2X03";
    for (int i = 0; i < 30; ++i) t.receive(std::to_string(i) + " " + text + "\r\n");
    // Lines 0-6 have scrolled off, after the 23 blank rows above the cursor.
    t.term.setViewOffset(7);
    for (int i = 0; i < 24; ++i) CHECK_EQ(t.row(i), std::to_string(i) + " " + text);
}

TEST_CASE("Clearing the screen doesn't fill the scrollback") {
    Terminal t;
    t.receive("one\r\ntwo" + ESC + "E" + "three");
    CHECK_EQ(t.term.scrollbackLines(), 0);
}

int main(int argc, char** argv) {
    if (argc != 2) {
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
