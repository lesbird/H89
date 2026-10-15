// Headless H89: the console serial port is connected to this terminal.
//
//   h89run --rom MTR90.ROM [--h17rom 444-19.ROM] [--sw501 hex] [--ram kb]
//          [--h19rom 444-46.ROM --font 444-29.ROM] [--disk0 f.h8d] [--disk1 f.h8d]
//          [--seconds n] [--type text] [--step "wait|keys"]... [--expect text]
//          [--screenshot f.pgm] [--trace-io]
//
// With --h19rom and --font the built-in H19 terminal is emulated: typing goes
// through its keyboard and --expect / the final printout use its screen.
// Without them the console serial port is wired straight to this terminal.
//
// With --seconds the machine runs up to that much emulated time as fast as
// possible, typing --type (\r for Return) once the ROM is waiting for input,
// then exits 0 if the output contains --expect. Each --step waits for its
// text to appear in the computer's output, then types its keys; steps run in
// order, and the run ends early once they are done and --expect is seen. Without it, it runs in real
// time interactively; press Ctrl-] to quit.

#include "Machine.h"
#include "cpu/Z80Disasm.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

[[noreturn]] void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s --rom <monitor.rom> [--h17rom <rom>] [--sw501 hex] [--ram kb]\n"
                 "          [--h19rom <rom> --font <rom>] [--disk0 f.h8d] [--disk1 f.h8d] [--ro]\n"
                 "          [--seconds n] [--type text] [--step \"wait|keys\"]... [--expect text]\n"
                 "          [--screenshot f.pgm] [--trace-io]\n",
                 argv0);
    std::exit(2);
}

std::string unescape(const char* s) {
    std::string out;
    for (; *s; ++s) {
        if (*s == '\\' && s[1] == 'r') {
            out += '\r';
            ++s;
        } else if (*s == '\\' && s[1] == 'n') {
            out += '\n';
            ++s;
        } else {
            out += *s;
        }
    }
    return out;
}

#if !defined(_WIN32)
struct RawTerminal {
    termios saved{};
    bool active = false;
    RawTerminal() {
        if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved) != 0) return;
        termios raw = saved;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG);
        raw.c_iflag &= ~(ICRNL | IXON);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        active = true;
    }
    ~RawTerminal() {
        if (active) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    }
};
#endif

} // namespace

int main(int argc, char** argv) {
    h89::MachineConfig config;
    double seconds = 0;
    std::string typeText, expect, screenshot;
    std::vector<std::pair<std::string, std::string>> steps;
    bool traceIo = false;
    bool readOnly = false;
    std::string diskPaths[3];

    for (int i = 1; i < argc; ++i) {
        auto arg = [&](const char* name) { return std::strcmp(argv[i], name) == 0 && i + 1 < argc; };
        std::string err;
        if (arg("--rom")) {
            config.monitorRom = h89::loadFile(argv[++i], &err);
        } else if (arg("--h17rom")) {
            config.h17Rom = h89::loadFile(argv[++i], &err);
        } else if (arg("--h19rom")) {
            config.terminal.rom = h89::loadFile(argv[++i], &err);
        } else if (arg("--font")) {
            config.terminal.font = h89::loadFile(argv[++i], &err);
        } else if (arg("--disk0")) {
            diskPaths[0] = argv[++i];
        } else if (arg("--disk1")) {
            diskPaths[1] = argv[++i];
        } else if (arg("--disk2")) {
            diskPaths[2] = argv[++i];
        } else if (std::strcmp(argv[i], "--ro") == 0) {
            readOnly = true;
        } else if (arg("--screenshot")) {
            screenshot = argv[++i];
        } else if (arg("--sw501")) {
            config.sw501 = static_cast<uint8_t>(std::strtoul(argv[++i], nullptr, 16));
        } else if (arg("--ram")) {
            config.ramKB = std::atoi(argv[++i]);
        } else if (arg("--seconds")) {
            seconds = std::atof(argv[++i]);
        } else if (arg("--type")) {
            typeText = unescape(argv[++i]);
        } else if (arg("--step")) {
            std::string step = unescape(argv[++i]);
            size_t bar = step.find('|');
            if (bar == std::string::npos) usage(argv[0]);
            steps.emplace_back(step.substr(0, bar), step.substr(bar + 1));
        } else if (arg("--expect")) {
            expect = unescape(argv[++i]);
        } else if (std::strcmp(argv[i], "--trace-io") == 0) {
            traceIo = true;
        } else {
            usage(argv[0]);
        }
        if (!err.empty()) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 2;
        }
    }
    if (config.monitorRom.empty()) usage(argv[0]);

    h89::Machine machine(config);
    h89::H19* terminal = machine.terminal();
    for (int d = 0; d < 3; ++d) {
        if (diskPaths[d].empty()) continue;
        std::string err;
        if (!machine.h17()) {
            std::fprintf(stderr, "--disk%d needs --h17rom\n", d);
            return 2;
        }
        if (!machine.h17()->insertDisk(d, diskPaths[d], readOnly, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 2;
        }
    }
    std::string output;
    machine.consoleOutput = [&](uint8_t b) {
        output += static_cast<char>(b & 0x7F);
        if (!terminal || seconds <= 0) {
            std::putchar(b & 0x7F);
            std::fflush(stdout);
        }
    };
    auto typeKey = [&](uint8_t c) {
        if (terminal) terminal->typeChar(c);
        else machine.consoleInput(c);
    };
    auto inputIdle = [&] { return terminal ? terminal->keyboardIdle() : machine.consoleInputIdle(); };
    if (traceIo) {
        machine.ioTrace = [&](bool write, uint8_t port, uint8_t value) {
            std::fprintf(stderr, "[%04X] %s %02Xh %s %02Xh\n", machine.cpu().regs().pc,
                         write ? "OUT" : "IN ", port, write ? "<-" : "->", value);
        };
    }

    if (seconds > 0) {
        // Scripted: type once the console has been idle for 0.5 s, i.e. the
        // ROM has printed its prompt and is waiting.
        const int slice = h89::Machine::kCpuHz / 1000;
        const int total = static_cast<int>(seconds * 1000);
        size_t typed = 0, lastLen = 0;
        int idleMs = 0;
        size_t step = 0, searchFrom = 0;
        std::string pending; // keys from a step, typed one at a time
        for (int ms = 0; ms < total; ++ms) {
            machine.run(slice);
            idleMs = output.size() == lastLen ? idleMs + 1 : 0;
            lastLen = output.size();
            if (typed < typeText.size() && idleMs >= 500 && inputIdle()) {
                typeKey(static_cast<uint8_t>(typeText[typed++]));
                idleMs = 400; // keep typing at a brisk pace once started
            }
            if (pending.empty() && step < steps.size()) {
                size_t at = output.find(steps[step].first, searchFrom);
                if (at != std::string::npos) {
                    searchFrom = at + steps[step].first.size();
                    pending = steps[step++].second;
                }
            }
            if (!pending.empty() && inputIdle()) {
                typeKey(static_cast<uint8_t>(pending.front()));
                pending.erase(0, 1);
            }
            bool scriptDone = typed == typeText.size() && step == steps.size() && pending.empty();
            if (scriptDone && !expect.empty() && output.find(expect, searchFrom) != std::string::npos &&
                ms % 500 == 0)
                break; // let the screen settle briefly, then stop
        }
        std::string screen;
        if (terminal) {
            screen = terminal->screenText();
            std::printf("+%s+\n", std::string(80, '-').c_str());
            size_t pos = 0, nl;
            while ((nl = screen.find('\n', pos)) != std::string::npos) {
                std::printf("|%-80s|\n", screen.substr(pos, nl - pos).c_str());
                pos = nl + 1;
            }
            std::printf("+%s+\n", std::string(80, '-').c_str());
        } else {
            std::printf("\n");
        }
        if (!screenshot.empty()) {
            const auto& fb = machine.framebuffer();
            std::ofstream out(screenshot, std::ios::binary);
            out << "P5 " << h89::kScreenWidth << " " << h89::kScreenHeight << " 255\n";
            out.write(reinterpret_cast<const char*>(fb.pixels.data()), fb.pixels.size());
        }
        if (!expect.empty()) {
            bool found = output.find(expect) != std::string::npos ||
                         (terminal && screen.find(expect) != std::string::npos);
            std::fprintf(stderr, "%s: expected \"%s\"\n", found ? "PASS" : "FAIL", expect.c_str());
            return found ? 0 : 1;
        }
        return 0;
    }

#if defined(_WIN32)
    std::fprintf(stderr, "interactive mode is not supported on Windows; use --seconds\n");
    return 2;
#else
    RawTerminal raw;
    std::fprintf(stderr, "[h89run: Ctrl-] to quit]\r\n");
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    for (;;) {
        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        while (inputIdle() && poll(&pfd, 1, 0) > 0) {
            char c;
            if (read(STDIN_FILENO, &c, 1) != 1) break;
            if (c == 0x1D) return 0; // Ctrl-]
            typeKey(static_cast<uint8_t>(c == '\n' ? '\r' : c));
        }
        machine.run(h89::Machine::kCpuHz / 100); // 10 ms slices
        next += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next);
    }
#endif
}
