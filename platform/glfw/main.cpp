// Windows (and Linux) front end: GLFW window + OpenGL 3.3 core.
//
//   H89 [--printer file] [--modem spec] [--aux spec] [rom-folder] [disk-image...]
//
// Serial ports (LP at E0h, MODEM at D8h, AUX at D0h) connect to a file,
// "listen:PORT" (a TCP client such as telnet) or "connect:HOST:PORT".
//
// The ROMs come from the folder given, the one chosen in the settings, or
// the roms folder installed next to the program. The first time it runs,
// the disks installed next to the program are copied to Documents\H89\Disks
// and put in the drives. Disk images given on the command line (or dropped
// on the window) go in instead: H17 images (.h8d) fill SY0:-SY2: in order and
// H37 images (.h37, .imd) DK0:-DK3:; if the first image is an H37 one the
// machine boots from the H37. Settings are kept in %APPDATA%\H89.

#include "GL.h" // must precede glfw3.h so GLFW doesn't pull in its own GL header
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "App.h"
#include "RomSet.h"

#if H89_DEBUGGER
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#endif

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

App* gApp = nullptr; // for the native dialogs' owner window

std::string diskKey(int drive) { return "disk" + std::to_string(drive); }
std::string serialKey(int port) { return "serial" + std::to_string(port); }

void playSound(const std::vector<uint8_t>& wav) {
#if defined(_WIN32)
    PlaySoundA(reinterpret_cast<LPCSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
#else
    (void)wav;
#endif
}

void systemBeep() {
#if defined(_WIN32)
    MessageBeep(MB_OK);
#else
    std::fputc('\a', stderr);
#endif
}

void ringBell(App& app) {
    // The firmware can ring in bursts (e.g. a full key buffer); one per burst.
    double now = glfwGetTime();
    if (app.bell == 2 || now - app.lastBell < 0.15) return;
    app.lastBell = now;
    if (app.bell == 1) systemBeep();
    else playSound(app.bellSound);
}

void keyClick(App& app) {
    if (app.keyClick && (app.speed == 1 || app.terminal)) playSound(app.clickSound);
}

// True while an ImGui window is taking the keyboard.
bool overlayHasKeyboard(const App& app) {
#if H89_DEBUGGER
    bool transfer = app.link && app.link->transfer();
    return (app.debuggerVisible || app.settingsVisible || transfer) && ImGui::GetIO().WantCaptureKeyboard;
#else
    (void)app;
    return false;
#endif
}

bool keypadKey(int key, h89::H19Key& out) {
    if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) {
        out = static_cast<h89::H19Key>(static_cast<int>(h89::H19Key::Keypad0) + (key - GLFW_KEY_KP_0));
        return true;
    }
    if (key == GLFW_KEY_KP_DECIMAL) { out = h89::H19Key::KeypadDot; return true; }
    if (key == GLFW_KEY_KP_ENTER) { out = h89::H19Key::KeypadEnter; return true; }
    return false;
}

void setSpeed(App& app, int speed) {
    app.settings.setInt("speed", speed);
    app.settings.save();
    applySettings(app);
}

void onKey(GLFWwindow* window, int key, int /*scancode*/, int action, int mods) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    const int hostKeys = GLFW_MOD_CONTROL | GLFW_MOD_ALT;
    if (overlayHasKeyboard(*app) && (mods & hostKeys) != hostKeys) return;
    h89::H19& term = h19(*app);

    if (key == GLFW_KEY_F10) { // BREAK is held, not typed
        if (action != GLFW_REPEAT) term.setBreak(action == GLFW_PRESS);
        return;
    }
    if (action == GLFW_RELEASE) return;

    // Host shortcuts use Ctrl+Alt so plain keys stay free for the H19 keyboard.
    if ((mods & hostKeys) == hostKeys) {
        auto setPhosphor = [app](int p) {
            app->settings.setInt("phosphor", p);
            app->settings.save();
            applySettings(*app);
        };
        // A terminal on its own has no computer to speed up, debug or save.
        bool computer = !terminalOnly(*app);
        switch (key) {
        case GLFW_KEY_R:
            if (computer) app->machine->reset();
            else term.reset();
            break;
        case GLFW_KEY_C:
            app->settings.setBool("crt", !app->renderer.effects());
            app->settings.save();
            applySettings(*app);
            break;
#if H89_DEBUGGER
        case GLFW_KEY_D: app->debuggerVisible = computer && !app->debuggerVisible; break;
        case GLFW_KEY_P: app->settingsVisible = !app->settingsVisible; break;
#endif
        case GLFW_KEY_T:
            if (computer) setSpeed(*app, app->speed == 0 ? app->lastSpeed : 0);
            break;
        case GLFW_KEY_S:
            if (computer) saveState(*app, defaultStatePath());
            break;
        case GLFW_KEY_L:
            if (computer) loadState(*app, defaultStatePath());
            break;
        case GLFW_KEY_1: setPhosphor(0); break;
        case GLFW_KEY_2: setPhosphor(1); break;
        case GLFW_KEY_3: setPhosphor(2); break;
        default: break;
        }
        return;
    }

    const bool shift = mods & GLFW_MOD_SHIFT;
    h89::H19Key special;
    if (keypadKey(key, special)) {
        term.pressKey(special, shift);
        app->suppressNextChar = true;
        return;
    }

    switch (key) {
    // The H19 has no arrow keys: the shifted keypad sends cursor motion.
    case GLFW_KEY_UP: term.pressKey(h89::H19Key::Keypad8, true); return;
    case GLFW_KEY_DOWN: term.pressKey(h89::H19Key::Keypad2, true); return;
    case GLFW_KEY_LEFT: term.pressKey(h89::H19Key::Keypad4, true); return;
    case GLFW_KEY_RIGHT: term.pressKey(h89::H19Key::Keypad6, true); return;
    case GLFW_KEY_HOME: term.pressKey(h89::H19Key::Keypad5, true); return;
    case GLFW_KEY_F1: term.pressKey(h89::H19Key::F1, shift); return;
    case GLFW_KEY_F2: term.pressKey(h89::H19Key::F2, shift); return;
    case GLFW_KEY_F3: term.pressKey(h89::H19Key::F3, shift); return;
    case GLFW_KEY_F4: term.pressKey(h89::H19Key::F4, shift); return;
    case GLFW_KEY_F5: term.pressKey(h89::H19Key::F5, shift); return;
    case GLFW_KEY_F6: term.pressKey(h89::H19Key::Erase, shift); return;
    case GLFW_KEY_F7: term.pressKey(h89::H19Key::Blue, shift); return;
    case GLFW_KEY_F8: term.pressKey(h89::H19Key::Red, shift); return;
    case GLFW_KEY_F9: term.pressKey(h89::H19Key::White, shift); return;
    case GLFW_KEY_PAGE_DOWN: term.pressKey(h89::H19Key::Scroll, shift); return;
    case GLFW_KEY_ENTER: term.typeChar(0x0D); return;
    case GLFW_KEY_BACKSPACE: term.typeChar(0x08); return;
    case GLFW_KEY_DELETE: term.typeChar(0x7F); return;
    case GLFW_KEY_TAB: term.typeChar(0x09); return;
    case GLFW_KEY_ESCAPE: term.typeChar(0x1B); return;
    default: break;
    }

    // GLFW sends no character events while Control is held.
    if ((mods & GLFW_MOD_CONTROL) && key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
        term.typeChar(static_cast<uint8_t>(key - GLFW_KEY_A + 1));
    }
}

bool isH17Image(const std::string& path) {
    std::string ext = path.size() >= 4 ? path.substr(path.size() - 4) : "";
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".h8d";
}

// Mounts images in order: H17 ones from SY0:, H37 ones from DK0:. The first
// image picks the boot device.
void insertDisks(App& app, const std::vector<std::string>& paths) {
    int h17 = 0, h37 = App::kFirstH37Drive;
    for (size_t i = 0; i < paths.size(); ++i) {
        bool hard = isH17Image(paths[i]);
        if (i == 0) {
            app.settings.setBool("bootH37", !hard);
            applySettings(app);
        }
        if (hard && h17 < App::kFirstH37Drive) insertDisk(app, h17++, paths[i], false);
        else if (!hard && h37 < App::kDrives) insertDisk(app, h37++, paths[i], false);
    }
}

void onDrop(GLFWwindow* window, int count, const char** paths) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    if (terminalOnly(*app)) return;
    insertDisks(*app, std::vector<std::string>(paths, paths + count));
}

void onChar(GLFWwindow* window, unsigned int codepoint) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    if (overlayHasKeyboard(*app)) return;
    if (app->suppressNextChar) {
        app->suppressNextChar = false;
        return;
    }
    if (codepoint >= 0x20 && codepoint < 0x7F) h19(*app).typeChar(static_cast<uint8_t>(codepoint));
}

// The first time the emulator runs, copies the disks that come with it to
// the user's disks folder and, if no disks are set yet, puts them in drives.
void installDisks(App& app) {
    if (app.bundledDisks.empty() || app.settings.getBool("disksInstalled", false)) return;
    std::string error;
    h89::copyBundledFiles(app.bundledDisks, app.disksDir, false, &error);
    if (!error.empty()) std::fprintf(stderr, "%s\n", error.c_str());
    app.settings.setBool("disksInstalled", true);
    for (int d = 0; d < App::kDrives; ++d)
        if (!app.settings.get(diskKey(d)).empty()) return;
    for (const auto& [drive, path] : h89::starterDisks(app.disksDir)) {
        app.settings.set(diskKey(drive), path);
        app.settings.setBool(diskKey(drive) + "ro", false);
    }
}

} // namespace

// --- App operations (also used by the settings panel) -----------------------------

void showError(const std::string& message) {
#if defined(_WIN32)
    MessageBoxA(gApp && gApp->window ? glfwGetWin32Window(gApp->window) : nullptr, message.c_str(), "H89",
                MB_OK | MB_ICONERROR);
#else
    std::fprintf(stderr, "%s\n", message.c_str());
#endif
}

std::string driveName(int drive) {
    return drive < App::kFirstH37Drive ? "SY" + std::to_string(drive) + ":"
                                       : "DK" + std::to_string(drive - App::kFirstH37Drive) + ":";
}

const TerminalSwitch kTerminalSwitches[] = {
    {"h19FullDuplex", "Full duplex (off: keys also show on the screen)", 401, h89::sw401::kFullDuplex, true},
    {"h19BlockCursor", "Block cursor", 402, h89::sw402::kBlockCursor, false},
    {"h19WrapAtEnd", "Wrap at the end of a line", 402, h89::sw402::kWrapAtEol, false},
    {"h19AutoLF", "Line feed after each received carriage return", 402, h89::sw402::kAutoLf, false},
    {"h19AutoCR", "Carriage return after each received line feed", 402, h89::sw402::kAutoCr, false},
    {"h19ANSI", "ANSI escape sequences (off: Heath)", 402, h89::sw402::kAnsiMode, false},
    {"h19KeypadShifted", "Keypad shifted", 402, h89::sw402::kKeypadShifted, false},
};
const int kTerminalSwitchCount = sizeof kTerminalSwitches / sizeof kTerminalSwitches[0];

h89::H19& h19(App& app) {
    return app.terminal ? *app.terminal : *app.machine->terminal();
}

bool terminalOnly(const App& app) {
    return app.terminal != nullptr;
}

std::string terminalSpec(const App& app) {
    if (!app.terminalOverride.empty()) return app.terminalOverride;
    const h89::Settings& s = app.settings;
    std::string port = s.get("terminalPort");
    if (port.empty()) return {};
    return port + ":" + std::to_string(s.getInt("terminalBaud", 9600)) + ":" + s.get("terminalFormat", "8N1") +
           (s.getBool("terminalRtsCts", false) ? ":rtscts" : "");
}

void connectTerminal(App& app) {
    if (!app.link) return;
    std::string spec = terminalSpec(app), error;
    h89::SerialSettings settings;
    if (spec.empty()) app.link->close();
    else if (!h89::SerialSettings::parse(spec, settings, &error)) showError(error);
    else app.link->open(settings); // the title shows any failure; it retries
    updateTitle(app);
}

bool driveAvailable(const App& app, int drive) {
    if (!app.machine) return false;
    return drive < App::kFirstH37Drive ? app.machine->h17() != nullptr : app.machine->h37() != nullptr;
}

std::string diskPath(const App& app, int drive) {
    if (!driveAvailable(app, drive)) return {};
    if (drive < App::kFirstH37Drive) {
        const auto& disk = app.machine->h17()->disk(drive);
        return disk.loaded() ? disk.path() : std::string();
    }
    const auto& disk = app.machine->h37()->disk(drive - App::kFirstH37Drive);
    return disk.loaded() ? disk.path() : std::string();
}

bool diskReadOnly(const App& app, int drive) {
    if (!driveAvailable(app, drive)) return false;
    if (drive < App::kFirstH37Drive) return app.machine->h17()->disk(drive).readOnly();
    return app.machine->h37()->disk(drive - App::kFirstH37Drive).readOnly();
}

// Inserts without touching the settings; false (with `error`) on failure.
static bool mount(App& app, int drive, const std::string& path, bool readOnly, std::string* error) {
    if (!driveAvailable(app, drive)) {
        *error = drive < App::kFirstH37Drive ? "This machine has no H17 controller (444-19 ROM missing)."
                                             : "This machine has no H37 controller.";
        return false;
    }
    return drive < App::kFirstH37Drive
               ? app.machine->h17()->insertDisk(drive, path, readOnly, error)
               : app.machine->h37()->insertDisk(drive - App::kFirstH37Drive, path, readOnly, error);
}

bool insertDisk(App& app, int drive, const std::string& path, bool readOnly) {
    std::string error;
    if (!mount(app, drive, path, readOnly, &error)) {
        showError(error);
        return false;
    }
    app.settings.set(diskKey(drive), path);
    app.settings.setBool(diskKey(drive) + "ro", readOnly);
    app.settings.save();
    updateTitle(app);
    return true;
}

void ejectDisk(App& app, int drive) {
    if (!driveAvailable(app, drive)) return;
    if (drive < App::kFirstH37Drive) app.machine->h17()->ejectDisk(drive);
    else app.machine->h37()->ejectDisk(drive - App::kFirstH37Drive);
    app.settings.remove(diskKey(drive));
    app.settings.remove(diskKey(drive) + "ro");
    app.settings.save();
    updateTitle(app);
}

void restoreOriginalDisks(App& app) {
    if (app.bundledDisks.empty()) return;
    // Take the copies out of the drives while they are replaced.
    std::vector<std::pair<int, bool>> remount;
    for (int d = 0; d < App::kDrives; ++d) {
        std::string path = diskPath(app, d);
        if (path.empty()) continue;
        std::error_code ec;
        if (fs::equivalent(fs::path(path).parent_path(), app.disksDir, ec) &&
            fs::exists(fs::path(app.bundledDisks) / fs::path(path).filename(), ec)) {
            remount.emplace_back(d, diskReadOnly(app, d));
            if (d < App::kFirstH37Drive) app.machine->h17()->ejectDisk(d);
            else app.machine->h37()->ejectDisk(d - App::kFirstH37Drive);
        }
    }
    std::string error;
    h89::copyBundledFiles(app.bundledDisks, app.disksDir, true, &error);
    for (const auto& [d, ro] : remount) {
        std::string err;
        mount(app, d, app.settings.get(diskKey(d)), ro, &err);
    }
    if (!error.empty()) showError(error);
}

bool connectSerial(App& app, int port, const std::string& spec) {
    if (!app.machine) return false;
    std::string error;
    if (spec.empty()) {
        app.serial[port].close();
        app.settings.remove(serialKey(port));
    } else if (app.serial[port].open(spec, &error)) {
        app.settings.set(serialKey(port), spec);
    } else {
        showError(error);
        return false;
    }
    app.serial[port].pump(*app.machine, port);
    app.settings.save();
    return true;
}

std::string defaultStatePath() {
    return (fs::path(h89::userDisksDir()).parent_path() / "H89.h89state").string();
}

void saveState(App& app, const std::string& path) {
    if (!app.machine) return;
    std::vector<uint8_t> state = app.machine->saveState();
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(state.data()), static_cast<std::streamsize>(state.size()));
    if (!out) showError("Couldn't write " + path);
}

void loadState(App& app, const std::string& path) {
    if (!app.machine) return;
    std::string err;
    std::vector<std::string> warnings;
    std::vector<uint8_t> state = h89::loadFile(path, &err);
    if (state.empty() || !app.machine->loadState(state, &err, &warnings)) {
        showError(path + ": " + err);
        return;
    }
    // The state brings its own disks and boot switch: remember them.
    for (int d = 0; d < App::kDrives; ++d) {
        std::string p = diskPath(app, d);
        if (p.empty()) {
            app.settings.remove(diskKey(d));
        } else {
            app.settings.set(diskKey(d), p);
            app.settings.setBool(diskKey(d) + "ro", diskReadOnly(app, d));
        }
    }
    app.settings.setBool("bootH37", (app.machine->config().sw501 & h89::sw501::kBootFrom170) != 0);
    app.settings.save();
    updateTitle(app);
    for (const auto& w : warnings) showError(w);
}

bool buildMachine(App& app, std::string* error) {
    std::string dir = app.romOverride;
    if (dir.empty()) dir = app.settings.get("roms");
    if (dir.empty()) dir = app.builtInRoms;
    bool mtr89 = app.settings.getInt("monitor", 90) == 89;
    bool terminal = !app.terminalOverride.empty() || app.settings.getBool("terminalOnly", false);
    h89::RomScan scan = h89::scanRomDirectory(dir, mtr89);
    // A terminal on its own needs only the H19's ROMs.
    auto usable = [&] { return terminal ? scan.config.hasTerminal() : scan.ok(); };
    if (!usable() && !app.builtInRoms.empty() && dir != app.builtInRoms) {
        showError(scan.error + "\n\nUsing the ROMs that came with H89 instead.");
        app.romOverride.clear();
        app.settings.remove("roms");
        app.settings.save();
        dir = app.builtInRoms;
        scan = h89::scanRomDirectory(dir, mtr89);
    }
    if (!usable()) {
        *error = scan.error;
        return false;
    }
    app.romDir = dir;
    app.romSummary = scan.summary;
    app.debug = h89::DebugControl();
    if (terminal) {
        app.link.reset(); // before the H19 it points at
        app.machine.reset();
#if H89_DEBUGGER
        app.debuggerVisible = false;
#endif
        app.terminal = std::make_unique<h89::H19>(scan.config.terminal);
        app.link = std::make_unique<h89::TerminalLink>(*app.terminal);
        app.terminal->onBell = [&app] { ringBell(app); };
        app.terminal->onKeyClick = [&app] { keyClick(app); };
        applySettings(app);
        connectTerminal(app);
        return true;
    }
    app.link.reset();
    app.terminal.reset();
    scan.config.ramKB = app.settings.getInt("memory", 64);
    app.machine = std::make_unique<h89::Machine>(std::move(scan.config));
    app.machine->terminal()->onBell = [&app] { ringBell(app); };
    app.machine->terminal()->onKeyClick = [&app] { keyClick(app); };
    app.machine->serialOutput = [&app](int port, uint8_t b) { app.serial[port].output(b); };
    app.machine->serialSource = [&app](int port) { return app.serial[port].next(); };
    applySettings(app);
    for (int d = 0; d < App::kDrives; ++d) {
        std::string path = app.settings.get(diskKey(d));
        std::string err;
        if (!path.empty() && !mount(app, d, path, app.settings.getBool(diskKey(d) + "ro", false), &err))
            app.settings.remove(diskKey(d));
    }
    app.settings.save();
    updateTitle(app);
    return true;
}

void applySettings(App& app) {
    const h89::Settings& s = app.settings;
    app.speed = s.getInt("speed", 1);
    if (app.speed) app.lastSpeed = app.speed;
    int phosphor = s.getInt("phosphor", 0);
    app.renderer.setPhosphor(phosphor == 1   ? h89::GLRenderer::Phosphor::Green
                             : phosphor == 2 ? h89::GLRenderer::Phosphor::Amber
                                             : h89::GLRenderer::Phosphor::White);
    app.renderer.setEffects(s.getBool("crt", true));
    app.bell = s.getInt("bell", 0);
    app.keyClick = s.getBool("keyClick", true);
    if (app.machine || app.terminal) {
        // The baud rate bits matter only inside an H89, whose console runs
        // at 9600; on a host serial port the port's rate is used.
        uint8_t sw401 = h89::sw401::kBaud9600, sw402 = 0;
        for (int i = 0; i < kTerminalSwitchCount; ++i) {
            const TerminalSwitch& t = kTerminalSwitches[i];
            if (s.getBool(t.key, t.fallback)) (t.sw == 401 ? sw401 : sw402) |= t.bit;
        }
        h89::H19& term = h19(app);
        if (term.config().sw401 != sw401 || term.config().sw402 != sw402) {
            term.setSwitches(sw401, sw402);
            term.reset();
        }
    }
    if (app.machine) {
        uint8_t sw = app.machine->config().sw501 & ~(h89::sw501::kBootFrom170 | h89::sw501::kAutoBoot);
        if (s.getBool("bootH37", false)) sw |= h89::sw501::kBootFrom170;
        if (s.getBool("autoBoot", false)) sw |= h89::sw501::kAutoBoot;
        app.machine->setSw501(sw);
    }
    updateTitle(app);
}

void updateTitle(App& app) {
    if (!app.window || (!app.machine && !app.terminal)) return;
    std::string title = "Heathkit H89";
    if (app.terminal) {
        std::string spec = terminalSpec(app);
        title = "H19 Terminal - " + (spec.empty() ? std::string("choose a serial port") : app.link->description());
#if H89_DEBUGGER
        title += "    [Ctrl+Alt+P: settings]";
#endif
        glfwSetWindowTitle(app.window, title.c_str());
        return;
    }
    int boot = app.settings.getBool("bootH37", false) ? App::kFirstH37Drive : 0;
    std::string path = diskPath(app, boot);
    if (!path.empty()) title += " - " + driveName(boot) + " " + fs::path(path).filename().string();
    if (app.speed == 0) title += " (turbo)";
#if H89_DEBUGGER
    title += "    [Ctrl+Alt+P: settings]";
#endif
    glfwSetWindowTitle(app.window, title.c_str());
}

#if defined(_WIN32)
static HWND ownerWindow() {
    return gApp && gApp->window ? glfwGetWin32Window(gApp->window) : nullptr;
}

std::string chooseOpenFile(const char* title, const char* filter) {
    char buf[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = ownerWindow();
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? buf : "";
}

std::vector<std::string> chooseOpenFiles(const char* title, const char* filter) {
    // Several files come back as "folder\0name\0name\0\0", one as its path.
    std::vector<char> buf(32768, 0);
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = ownerWindow();
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameA(&ofn)) return {};
    std::vector<std::string> parts;
    for (const char* p = buf.data(); *p; p += std::strlen(p) + 1) parts.emplace_back(p);
    if (parts.size() == 1) return parts;
    std::vector<std::string> paths;
    for (size_t i = 1; i < parts.size(); ++i) paths.push_back((fs::path(parts[0]) / parts[i]).string());
    return paths;
}

std::string chooseSaveFile(const char* title, const char* filter, const char* defaultName) {
    char buf[MAX_PATH];
    std::snprintf(buf, sizeof buf, "%s", defaultName);
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = ownerWindow();
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameA(&ofn) ? buf : "";
}

std::string chooseFolder(const char* title) {
    char buf[MAX_PATH];
    BROWSEINFOA bi = {};
    bi.hwndOwner = ownerWindow();
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return "";
    bool ok = SHGetPathFromIDListA(pidl, buf);
    CoTaskMemFree(pidl);
    return ok ? buf : "";
}

void openFolder(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
#else
std::string chooseOpenFile(const char*, const char*) { return ""; }
std::vector<std::string> chooseOpenFiles(const char*, const char*) { return {}; }
std::string chooseSaveFile(const char*, const char*, const char*) { return ""; }
std::string chooseFolder(const char*) { return ""; }
void openFolder(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
#if defined(__APPLE__)
    std::string cmd = "open \"" + path + "\"";
#else
    std::string cmd = "xdg-open \"" + path + "\"";
#endif
    if (std::system(cmd.c_str()) != 0) showError("Couldn't open " + path);
}
#endif

int main(int argc, char** argv) {
    const char* usage = "Usage: H89 [--printer file] [--modem spec] [--aux spec] [rom-folder] [disk-image...]\n"
                        "       H89 --terminal PORT[:BAUD[:FORMAT[:rtscts]]] [rom-folder]   (H19 terminal only)";
#if defined(_WIN32)
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // for the folder picker
#endif
    App app;
    gApp = &app;
    app.settings = h89::Settings((fs::path(h89::userDataDir()) / "settings.ini").string());
    app.settings.load();
    app.builtInRoms = h89::findBundledDir("roms");
    app.bundledDisks = h89::findBundledDir("disks");
    app.disksDir = h89::userDisksDir();
    app.bellSound = h89::makeBellSound();
    app.clickSound = h89::makeKeyClickSound();

    std::string serialSpecs[h89::Machine::kSerialPorts];
    std::vector<std::string> disks;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        int port = a == "--printer" ? h89::Machine::kPrinter
                 : a == "--modem"   ? h89::Machine::kModem
                 : a == "--aux"     ? h89::Machine::kAux
                                    : -1;
        std::error_code ec;
        if (port >= 0 && i + 1 < argc) serialSpecs[port] = argv[++i];
        else if (a == "--terminal" && i + 1 < argc) app.terminalOverride = argv[++i];
        else if (a == "--help" || a == "-h") {
            showError(usage);
            return 0;
        } else if (disks.empty() && app.romOverride.empty() && fs::is_directory(a, ec)) app.romOverride = a;
        else disks.push_back(a);
    }
    installDisks(app);

    glfwSetErrorCallback([](int code, const char* msg) { std::fprintf(stderr, "GLFW %d: %s\n", code, msg); });
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(960, 720, "Heathkit H89", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    auto getProc = [](const char* name) -> void* { return reinterpret_cast<void*>(glfwGetProcAddress(name)); };
    if (!gl::loadFunctions(getProc)) {
        showError("OpenGL 3.3 is not available.");
        glfwTerminate();
        return 1;
    }
    if (!app.renderer.init()) {
        glfwTerminate();
        return 1;
    }
    app.window = window;

    std::string error;
    if (!buildMachine(app, &error)) {
        showError(error + "\n\n" + usage);
        app.renderer.shutdown();
        glfwTerminate();
        return 1;
    }
    if (!disks.empty() && app.machine) insertDisks(app, disks);
    for (int p = 0; p < h89::Machine::kSerialPorts && app.machine; ++p) {
        std::string spec = serialSpecs[p].empty() ? app.settings.get(serialKey(p)) : serialSpecs[p];
        if (!spec.empty()) connectSerial(app, p, spec);
    }

    glfwSetWindowUserPointer(window, &app);
    glfwSetKeyCallback(window, onKey);
    glfwSetCharCallback(window, onChar);
    glfwSetDropCallback(window, onDrop);
#if H89_DEBUGGER
    // After our callbacks, so the ImGui backend chains to them.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");
    // The first time, open the settings so the shortcuts and disks are seen.
    if (!app.settings.getBool("welcomed", false)) {
        app.settingsVisible = true;
        app.settings.setBool("welcomed", true);
        app.settings.save();
    }
#endif
    updateTitle(app);

    // Fixed 60 Hz emulation, decoupled from the display refresh rate.
    constexpr double kFrameTime = 1.0 / h89::Machine::kFrameHz;
    double last = glfwGetTime();
    double pending = 0.0;
    double lastTitle = last;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        double now = glfwGetTime();
        pending += now - last;
        last = now;
        if (app.terminal) {
            // Real time; the link catches up on its own when data is waiting.
            if (pending > 0.25) pending = 0.25;
            for (; pending >= kFrameTime; pending -= kFrameTime) app.link->runFrame();
            if (now - lastTitle >= 1.0) { // the connection can come and go
                lastTitle = now;
                updateTitle(app);
            }
        } else {
            for (int p = 0; p < h89::Machine::kSerialPorts; ++p) app.serial[p].pump(*app.machine, p);
            if (app.speed == 0) {
                // As many frames as fit in most of a display refresh.
                do app.debug.runFrame(*app.machine);
                while (glfwGetTime() - now < 0.012);
                pending = 0;
            }
            if (pending > 0.25) pending = 0.25; // don't spiral after a stall
            while (pending >= kFrameTime) {
                for (int i = 0; i < app.speed; ++i) app.debug.runFrame(*app.machine);
                pending -= kFrameTime;
            }
        }

        const auto& fb = h19(app).framebuffer();
        app.renderer.uploadFrame(fb.pixels.data(), h89::kScreenWidth, h89::kScreenHeight);
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        app.renderer.draw(w, h);
#if H89_DEBUGGER
        if (app.machine && app.debug.paused()) app.debuggerVisible = true; // e.g. a breakpoint was hit
        bool transfer = app.link && app.link->transfer();
        if (app.debuggerVisible || app.settingsVisible || transfer) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            if (app.debuggerVisible && app.machine) app.debuggerUI.draw(*app.machine, app.debug);
            if (app.settingsVisible) drawSettings(app); // may rebuild the machine
            if (app.link && app.link->transfer()) drawTransfer(app);
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }
#endif
        glfwSwapBuffers(window);
    }
#if H89_DEBUGGER
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
#endif
    app.renderer.shutdown();
    app.link.reset();
    app.terminal.reset();
    app.machine.reset();
    gApp = nullptr;

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
