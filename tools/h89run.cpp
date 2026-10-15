// Headless H89 in a text terminal.
//
//   h89run [--roms dir] [--disk0 f.h8d] [--disk1 f.h8d] [--disk2 f.h8d] [--ro]
//          [--dk0 f.h37|f.imd] ... [--dk3 ...]   (H37 drives)
//          [--rom MTR90.ROM] [--h17rom 444-19.ROM] [--h19rom 444-46.ROM --font 444-29.ROM]
//          [--sw501 hex] [--ram kb]
//          [--seconds n] [--type text] [--step "wait|keys"]... [--expect text]
//          [--screenshot f.pgm] [--trace-io] [--load-state f] [--save-state f]
//          [--printer file] [--modem spec] [--aux spec]
//   h89run --terminal PORT[:BAUD[:FORMAT[:rtscts]]] [--roms dir] [--seconds n] [--type text]
//          [--step "wait|keys"]... [--expect text] [transfer]
//   transfer: --send-text f | --xmodem-send f | --xmodem1k-send f | --zmodem-send f...
//             | --xmodem-receive f | --zmodem-receive dir
//
// With no ROM options, the ROMs are found by part number in the first folder
// of: --roms, $H89_ROMS, ./roms, and roms/ next to (or one level above) the
// executable. Giving --rom instead loads only the ROMs named on the command
// line.
//
// With the H19 ROMs the built-in terminal is emulated: typing goes through its
// keyboard, the interactive mode mirrors its screen, and --expect / the final
// printout use its screen. Without them the console serial port is wired
// straight to this terminal.
//
// With --seconds the machine runs up to that much emulated time as fast as
// possible, typing --type (\r for Return) once the ROM is waiting for input,
// then exits 0 if the output contains --expect. Each --step waits for its
// text to appear in the computer's output, then types its keys; steps run in
// order, and the run ends early once they are done and --expect is seen. Without it, it runs in real
// time interactively; press Ctrl-] to quit.
//
// --load-state starts from a saved machine state (remounting its disks);
// --save-state writes one when the run ends.
//
// The serial card's ports (LP at E0h, MODEM at D8h, AUX at D0h) connect to a
// file, "listen:PORT" (a TCP client such as telnet) or "connect:HOST:PORT".
//
// --terminal runs just the H19, as a terminal for a real computer on a host
// serial port (/dev/cu.usbserial-1410:115200:8N1, say), at that port's rate.
// --seconds then runs for that much real time; --step and --expect watch
// what the computer sends. A file transfer starts once the steps are done
// (so they can start the computer's end), and the run waits for it; the exit
// status says whether it worked.

#include "Machine.h"
#include "RomSet.h"
#include "SerialHost.h"
#include "TerminalLink.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
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
                 "usage: %s [--roms dir] [--disk0 f.h8d] [--disk1 f.h8d] [--disk2 f.h8d] [--ro]\n"
                 "          [--dk0 f.h37|f.imd] [--dk1 ...] [--dk2 ...] [--dk3 ...]\n"
                 "          [--rom <monitor.rom>] [--h17rom <rom>] [--h19rom <rom> --font <rom>]\n"
                 "          [--sw501 hex] [--ram kb]\n"
                 "          [--seconds n] [--type text] [--step \"wait|keys\"]... [--expect text]\n"
                 "          [--screenshot f.pgm] [--trace-io]\n"
                 "       %s --terminal PORT[:BAUD[:FORMAT[:rtscts]]] [--roms dir]\n"
                 "          [--seconds n] [--type text] [--step \"wait|keys\"]... [--expect text]\n"
                 "          [--send-text f | --xmodem-send f | --xmodem1k-send f | --zmodem-send f...\n"
                 "           | --xmodem-receive f | --zmodem-receive dir]\n",
                 argv0, argv0);
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

namespace fs = std::filesystem;

// Where to look for the ROM folder when no ROM options are given.
std::vector<fs::path> romSearchPath(const char* argv0, const std::string& explicitDir) {
    if (!explicitDir.empty()) return {explicitDir};
    std::vector<fs::path> dirs;
    if (const char* env = std::getenv("H89_ROMS")) dirs.emplace_back(env);
    dirs.emplace_back("roms");
    std::error_code ec;
    fs::path exe = fs::weakly_canonical(fs::absolute(argv0, ec), ec);
    if (!ec && exe.has_parent_path()) {
        dirs.push_back(exe.parent_path() / "roms");
        dirs.push_back(exe.parent_path().parent_path() / "roms");
    }
    // Drop entries that name the same folder.
    std::vector<fs::path> unique;
    for (const auto& d : dirs) {
        fs::path norm = fs::weakly_canonical(fs::absolute(d, ec), ec);
        bool seen = false;
        for (const auto& u : unique) seen = seen || fs::weakly_canonical(fs::absolute(u, ec), ec) == norm;
        if (!seen) unique.push_back(d);
    }
    return unique;
}

#if !defined(_WIN32)
// Shows the H19's screen in this terminal (alternate screen buffer).
struct ScreenMirror {
    std::string lastText;
    int lastRow = -1, lastCol = -1;
    ScreenMirror() { std::fputs("\x1b]0;H89 (Ctrl-] quits)\x07\x1b[?1049h\x1b[2J", stdout); }
    ~ScreenMirror() {
        std::fputs("\x1b[?1049l", stdout);
        std::fflush(stdout);
    }
    void update(const h89::H19& term) {
        std::string text = term.screenText();
        int row = 0, col = 0;
        bool cursor = term.cursorPosition(row, col);
        if (text == lastText && row == lastRow && col == lastCol) return;
        lastText = text;
        lastRow = row;
        lastCol = col;
        std::string out = "\x1b[?25l\x1b[H";
        size_t pos = 0, nl;
        int line = 0;
        while ((nl = text.find('\n', pos)) != std::string::npos && line < 25) {
            if (line++) out += "\r\n";
            out += text.substr(pos, nl - pos) + "\x1b[K";
            pos = nl + 1;
        }
        out += "\x1b[J";
        if (cursor) out += "\x1b[" + std::to_string(row + 1) + ";" + std::to_string(col + 1) + "H\x1b[?25h";
        std::fputs(out.c_str(), stdout);
        std::fflush(stdout);
    }
};

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

void printScreen(const h89::H19& term) {
    std::string screen = term.screenText();
    std::printf("+%s+\n", std::string(80, '-').c_str());
    size_t pos = 0, nl;
    while ((nl = screen.find('\n', pos)) != std::string::npos) {
        std::printf("|%-80s|\n", screen.substr(pos, nl - pos).c_str());
        pos = nl + 1;
    }
    std::printf("+%s+\n", std::string(80, '-').c_str());
}

// A file transfer for --terminal: kind ("send-text", "xmodem-send", ...) and
// its files.
struct TransferRequest {
    std::string kind;
    std::vector<std::string> paths;
};

std::unique_ptr<h89::FileTransfer> makeTransfer(const TransferRequest& r, std::string* error) {
    if (r.kind == "send-text") return h89::sendTextFile(r.paths[0], {}, error);
    if (r.kind == "xmodem-send") return h89::xmodemSend(r.paths[0], false, error);
    if (r.kind == "xmodem1k-send") return h89::xmodemSend(r.paths[0], true, error);
    if (r.kind == "zmodem-send") return h89::zmodemSend(r.paths, error);
    if (r.kind == "xmodem-receive") return h89::xmodemReceive(r.paths[0], error);
    return h89::zmodemReceive();
}

// --terminal: the H19 on its own, on a host serial port, in real time.
int runTerminal(const h89::H19Config& config, const std::string& spec, double seconds, const std::string& typeText,
                const std::vector<std::pair<std::string, std::string>>& steps, const std::string& expect,
                const TransferRequest& transferRequest) {
    h89::SerialSettings settings;
    std::string err;
    if (!h89::SerialSettings::parse(spec, settings, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    h89::H19 term(config);
    h89::TerminalLink link(term);
    if (!link.open(settings, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        if (seconds > 0) return 2;
        std::fprintf(stderr, "(running offline; retrying the port every second)\n");
    }
    using clock = std::chrono::steady_clock;
    const auto frame = std::chrono::microseconds(1'000'000 / 60);
    auto next = clock::now();
    bool transferStarted = false;
    auto startTransfer = [&]() -> bool {
        transferStarted = true;
        auto t = makeTransfer(transferRequest, &err);
        if (!t) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
        link.startTransfer(std::move(t));
        return true;
    };
    // ZMODEM downloads go in the folder given.
    auto answerTransfer = [&] {
        h89::FileTransfer* t = link.transfer();
        if (t && t->state() == h89::FileTransfer::State::NeedsSavePath)
            t->setSavePath((std::filesystem::path(transferRequest.paths[0]) / t->offeredName()).string());
    };
    auto transferReport = [&]() -> int {
        h89::FileTransfer* t = link.transfer();
        if (!t) return 0;
        std::fprintf(stderr, "%s: %s\n", t->title().c_str(), t->status().c_str());
        for (const auto& f : t->savedFiles()) std::fprintf(stderr, "saved %s\n", f.c_str());
        return t->state() == h89::FileTransfer::State::Done ? 0 : 1;
    };

    if (seconds > 0) {
        std::string pending = typeText;
        size_t step = 0, searchFrom = 0;
        auto end = clock::now() + std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000));
        while (clock::now() < end) {
            link.runFrame();
            const std::string& got = link.received();
            if (pending.empty() && step < steps.size()) {
                size_t at = got.find(steps[step].first, searchFrom);
                if (at != std::string::npos) {
                    searchFrom = at + steps[step].first.size();
                    pending = steps[step++].second;
                }
            }
            if (!pending.empty() && term.keyboardIdle()) {
                term.typeChar(static_cast<uint8_t>(pending.front()));
                pending.erase(0, 1);
            }
            bool done = pending.empty() && step == steps.size();
            if (done && !transferRequest.kind.empty() && !transferStarted && !startTransfer()) return 2;
            answerTransfer();
            if (link.transferRunning()) done = false;
            if (done && !transferRequest.kind.empty()) {
                // Let the last reply go, then stop unless there's text to wait for.
                for (int i = 0; i < 30; ++i) link.runFrame();
                if (expect.empty()) break;
            }
            if (done && !expect.empty() && term.screenText().find(expect) != std::string::npos) break;
            next += frame;
            std::this_thread::sleep_until(next);
        }
        printScreen(term);
        std::fprintf(stderr, "%s\n", link.description().c_str());
        int transferResult = transferReport();
        if (!transferRequest.kind.empty() && !transferStarted) {
            std::fprintf(stderr, "the transfer never started (its steps didn't finish)\n");
            transferResult = 1;
        }
        if (expect.empty()) return transferResult;
        bool found = term.screenText().find(expect) != std::string::npos ||
                     link.received().find(expect) != std::string::npos;
        std::fprintf(stderr, "%s: expected \"%s\"\n", found ? "PASS" : "FAIL", expect.c_str());
        return found ? transferResult : 1;
    }

#if defined(_WIN32)
    std::fprintf(stderr, "interactive mode is not supported on Windows; use --seconds\n");
    return 2;
#else
    if (!transferRequest.kind.empty() && !startTransfer()) return 2;
    struct Report {
        std::function<int()> f;
        ~Report() { f(); }
    } report{transferReport}; // after the screen is restored
    RawTerminal raw;
    ScreenMirror mirror;
    for (;;) {
        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        while (term.keyboardIdle() && poll(&pfd, 1, 0) > 0) {
            char c;
            if (read(STDIN_FILENO, &c, 1) != 1) break;
            if (c == 0x1D) return 0; // Ctrl-]
            term.typeChar(static_cast<uint8_t>(c == '\n' ? '\r' : c));
        }
        link.runFrame();
        answerTransfer();
        mirror.update(term);
        next += frame;
        std::this_thread::sleep_until(next);
    }
#endif
}

} // namespace

int main(int argc, char** argv) {
    h89::MachineConfig config;
    double seconds = 0;
    std::string typeText, expect, screenshot;
    std::vector<std::pair<std::string, std::string>> steps;
    bool traceIo = false;
    bool readOnly = false;
    std::string diskPaths[3];
    std::string h37Paths[4];
    std::string romDir;
    std::string loadStatePath, saveStatePath;
    std::string serialSpecs[h89::Machine::kSerialPorts];
    std::string terminalSpec;
    TransferRequest transfer;

    for (int i = 1; i < argc; ++i) {
        auto arg = [&](const char* name) { return std::strcmp(argv[i], name) == 0 && i + 1 < argc; };
        std::string err;
        if (arg("--roms")) {
            romDir = argv[++i];
        } else if (arg("--rom")) {
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
        } else if (std::strncmp(argv[i], "--dk", 4) == 0 && argv[i][4] >= '0' && argv[i][4] <= '3' &&
                   argv[i][5] == 0 && i + 1 < argc) {
            int drive = argv[i][4] - '0';
            h37Paths[drive] = argv[++i];
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
        } else if (arg("--printer")) {
            serialSpecs[h89::Machine::kPrinter] = argv[++i];
        } else if (arg("--modem")) {
            serialSpecs[h89::Machine::kModem] = argv[++i];
        } else if (arg("--aux")) {
            serialSpecs[h89::Machine::kAux] = argv[++i];
        } else if (arg("--load-state")) {
            loadStatePath = argv[++i];
        } else if (arg("--save-state")) {
            saveStatePath = argv[++i];
        } else if (arg("--terminal")) {
            terminalSpec = argv[++i];
        } else if (arg("--send-text") || arg("--xmodem-send") || arg("--xmodem1k-send") || arg("--zmodem-send") ||
                   arg("--xmodem-receive") || arg("--zmodem-receive")) {
            std::string kind = argv[i] + 2;
            if (!transfer.kind.empty() && !(kind == "zmodem-send" && transfer.kind == kind)) usage(argv[0]);
            transfer.kind = kind;
            transfer.paths.push_back(argv[++i]);
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
    if (!transfer.kind.empty() && terminalSpec.empty()) {
        std::fprintf(stderr, "file transfers need --terminal\n");
        return 2;
    }
    if (!terminalSpec.empty()) {
        // Only the H19's ROMs are needed.
        if (!config.hasTerminal()) {
            for (const auto& dir : romSearchPath(argv[0], romDir)) {
                h89::RomScan scan = h89::scanRomDirectory(dir.string());
                if (!scan.config.hasTerminal()) continue;
                if (config.terminal.rom.empty()) config.terminal.rom = std::move(scan.config.terminal.rom);
                if (config.terminal.font.empty()) config.terminal.font = std::move(scan.config.terminal.font);
                break;
            }
        }
        if (!config.hasTerminal()) {
            std::fprintf(stderr, "The H19 ROMs (444-46 and 444-29) weren't found; use --roms <dir>.\n");
            return 2;
        }
        return runTerminal(config.terminal, terminalSpec, seconds, typeText, steps, expect, transfer);
    }
    if (config.monitorRom.empty()) {
        // No ROMs named: load the whole set from the first ROM folder found,
        // keeping any individual ROMs given on the command line.
        std::string tried;
        bool found = false;
        for (const auto& dir : romSearchPath(argv[0], romDir)) {
            h89::RomScan scan = h89::scanRomDirectory(dir.string());
            if (!scan.ok()) {
                tried += "\n  " + dir.string();
                continue;
            }
            config.monitorRom = std::move(scan.config.monitorRom);
            if (config.h17Rom.empty()) config.h17Rom = std::move(scan.config.h17Rom);
            if (config.terminal.rom.empty()) config.terminal.rom = std::move(scan.config.terminal.rom);
            if (config.terminal.font.empty()) config.terminal.font = std::move(scan.config.terminal.font);
            found = true;
            break;
        }
        if (!found) {
            std::fprintf(stderr,
                         "No complete ROM set found (monitor 444-142 or 444-62, H19 444-46 and 444-29).\n"
                         "Looked in:%s\nUse --roms <dir>, set H89_ROMS, or name ROMs with --rom etc.\n",
                         tried.c_str());
            return 2;
        }
    }

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
    for (int d = 0; d < 4; ++d) {
        if (h37Paths[d].empty()) continue;
        std::string err;
        if (!machine.h37()->insertDisk(d, h37Paths[d], readOnly, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 2;
        }
    }
    if (!loadStatePath.empty()) {
        std::string err;
        std::vector<std::string> warnings;
        std::vector<uint8_t> state = h89::loadFile(loadStatePath, &err);
        if (state.empty() || !machine.loadState(state, &err, &warnings)) {
            std::fprintf(stderr, "%s: %s\n", loadStatePath.c_str(), err.c_str());
            return 2;
        }
        for (const auto& w : warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    }
    h89::SerialHost serialHosts[h89::Machine::kSerialPorts];
    for (int p = 0; p < h89::Machine::kSerialPorts; ++p) {
        std::string err;
        if (!serialSpecs[p].empty() && !serialHosts[p].open(serialSpecs[p], &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 2;
        }
    }
    machine.serialOutput = [&](int port, uint8_t b) { serialHosts[port].output(b); };
    machine.serialSource = [&](int port) { return serialHosts[port].next(); };
    auto pumpSerial = [&] {
        for (int p = 0; p < h89::Machine::kSerialPorts; ++p) serialHosts[p].pump(machine, p);
    };
    pumpSerial();
    auto saveState = [&] {
        if (saveStatePath.empty()) return;
        std::vector<uint8_t> state = machine.saveState();
        std::ofstream out(saveStatePath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(state.data()), static_cast<std::streamsize>(state.size()));
        if (!out) std::fprintf(stderr, "cannot write %s\n", saveStatePath.c_str());
    };
    std::string output;
    machine.consoleOutput = [&](uint8_t b) {
        output += static_cast<char>(b & 0x7F);
        if (!terminal) {
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
            if (ms % 16 == 0) pumpSerial();
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
        saveState();
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
    std::unique_ptr<ScreenMirror> mirror;
    if (terminal) mirror = std::make_unique<ScreenMirror>();
    else std::fprintf(stderr, "[h89run: Ctrl-] to quit]\r\n");
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    for (;;) {
        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        while (inputIdle() && poll(&pfd, 1, 0) > 0) {
            char c;
            if (read(STDIN_FILENO, &c, 1) != 1) break;
            if (c == 0x1D) { // Ctrl-]
                mirror.reset();
                saveState();
                return 0;
            }
            typeKey(static_cast<uint8_t>(c == '\n' ? '\r' : c));
        }
        machine.run(h89::Machine::kCpuHz / 100); // 10 ms slices
        pumpSerial();
        if (mirror) mirror->update(*terminal);
        next += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next);
    }
#endif
}
