// Windows (and Linux) front end: GLFW window + OpenGL 3.3 core.
//
//   H89-glfw [--printer file] [--modem spec] [--aux spec] [rom-folder [disk-image...]]
//
// Serial ports (LP at E0h, MODEM at D8h, AUX at D0h) connect to a file,
// "listen:PORT" (a TCP client such as telnet) or "connect:HOST:PORT".
//
// The ROM folder defaults to "roms" in the working directory. H17 images
// (.h8d) fill SY0:-SY2: in order and H37 images (.h37, .imd) DK0:-DK3:; if
// the first image is an H37 one the machine boots from the H37. Images can
// also be dropped on the window.

#include "GL.h" // must precede glfw3.h so GLFW doesn't pull in its own GL header
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "DebugControl.h"
#include "GLRenderer.h"
#include "SerialHost.h"

#if H89_DEBUGGER
#include "DebuggerUI.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#endif

#include <cctype>
#include <fstream>
#include <vector>
#include "Machine.h"
#include "RomSet.h"

#include <cstdio>
#include <memory>
#include <string>

namespace {

struct App {
    std::unique_ptr<h89::Machine> machine;
    h89::GLRenderer renderer;
    GLFWwindow* window = nullptr;
    bool suppressNextChar = false; // keypad keys also generate a char event
    bool turbo = false;            // run as fast as the host allows
    h89::SerialHost serial[h89::Machine::kSerialPorts];
    h89::DebugControl debug;
#if H89_DEBUGGER
    h89::DebuggerUI debuggerUI;
    bool debuggerVisible = false;
#endif
};

// True while the debugger overlay is taking the keyboard.
bool debuggerHasKeyboard(const App& app) {
#if H89_DEBUGGER
    return app.debuggerVisible && ImGui::GetIO().WantCaptureKeyboard;
#else
    (void)app;
    return false;
#endif
}

// Quick save/load file, in the working directory.
constexpr const char* kStateFile = "h89.state";

void updateTitle(App& app) {
    glfwSetWindowTitle(app.window, app.turbo ? "Heathkit H89 (turbo)" : "Heathkit H89");
}

void showError(const std::string& message) {
#if defined(_WIN32)
    MessageBoxA(nullptr, message.c_str(), "H89", MB_OK | MB_ICONERROR);
#else
    std::fprintf(stderr, "%s\n", message.c_str());
#endif
}

void ringBell() {
#if defined(_WIN32)
    MessageBeep(MB_OK);
#else
    std::fputc('\a', stderr);
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

void onKey(GLFWwindow* window, int key, int /*scancode*/, int action, int mods) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    const int hostKeys = GLFW_MOD_CONTROL | GLFW_MOD_ALT;
    if (debuggerHasKeyboard(*app) && (mods & hostKeys) != hostKeys) return;
    h89::H19& term = *app->machine->terminal();

    if (key == GLFW_KEY_F10) { // BREAK is held, not typed
        if (action != GLFW_REPEAT) term.setBreak(action == GLFW_PRESS);
        return;
    }
    if (action == GLFW_RELEASE) return;

    // Host shortcuts use Ctrl+Alt so plain keys stay free for the H19 keyboard.
    const int hostMods = GLFW_MOD_CONTROL | GLFW_MOD_ALT;
    if ((mods & hostMods) == hostMods) {
        switch (key) {
        case GLFW_KEY_R: app->machine->reset(); break;
        case GLFW_KEY_C: app->renderer.setEffects(!app->renderer.effects()); break;
#if H89_DEBUGGER
        case GLFW_KEY_D: app->debuggerVisible = !app->debuggerVisible; break;
#endif
        case GLFW_KEY_T:
            app->turbo = !app->turbo;
            updateTitle(*app);
            break;
        case GLFW_KEY_S: {
            std::vector<uint8_t> state = app->machine->saveState();
            std::ofstream out(kStateFile, std::ios::binary);
            out.write(reinterpret_cast<const char*>(state.data()), static_cast<std::streamsize>(state.size()));
            if (!out) showError(std::string("Couldn't write ") + kStateFile);
            break;
        }
        case GLFW_KEY_L: {
            std::string err;
            std::vector<std::string> warnings;
            std::vector<uint8_t> state = h89::loadFile(kStateFile, &err);
            if (state.empty() || !app->machine->loadState(state, &err, &warnings)) {
                showError(std::string(kStateFile) + ": " + err);
            } else {
                for (const auto& w : warnings) showError(w);
            }
            break;
        }
        case GLFW_KEY_1: app->renderer.setPhosphor(h89::GLRenderer::Phosphor::White); break;
        case GLFW_KEY_2: app->renderer.setPhosphor(h89::GLRenderer::Phosphor::Green); break;
        case GLFW_KEY_3: app->renderer.setPhosphor(h89::GLRenderer::Phosphor::Amber); break;
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

// Mounts images in order: H17 ones from SY0:, H37 ones from DK0:.
void insertDisks(App& app, int count, const char* const* paths) {
    int h17 = 0, h37 = 0;
    for (int i = 0; i < count; ++i) {
        std::string err;
        bool hard = isH17Image(paths[i]);
        if (i == 0) {
            uint8_t sw = app.machine->config().sw501 & ~h89::sw501::kBootFrom170;
            app.machine->setSw501(hard ? sw : (sw | h89::sw501::kBootFrom170));
        }
        if (hard) {
            if (!app.machine->h17()) showError("This machine has no H17 controller (444-19 ROM missing).");
            else if (h17 < h89::H17::kDrives && !app.machine->h17()->insertDisk(h17++, paths[i], false, &err))
                showError(err);
        } else if (h37 < h89::H37::kDrives && !app.machine->h37()->insertDisk(h37++, paths[i], false, &err)) {
            showError(err);
        }
    }
}

void onDrop(GLFWwindow* window, int count, const char** paths) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    insertDisks(*app, count, paths);
}

void onChar(GLFWwindow* window, unsigned int codepoint) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    if (debuggerHasKeyboard(*app)) return;
    if (app->suppressNextChar) {
        app->suppressNextChar = false;
        return;
    }
    if (codepoint >= 0x20 && codepoint < 0x7F) app->machine->terminal()->typeChar(static_cast<uint8_t>(codepoint));
}

} // namespace

int main(int argc, char** argv) {
    const char* usage = "Usage: H89-glfw [--printer file] [--modem spec] [--aux spec] [rom-folder [disk-image...]]";
    std::string serialSpecs[h89::Machine::kSerialPorts];
    std::vector<const char*> positional;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        int port = a == "--printer" ? h89::Machine::kPrinter
                 : a == "--modem"   ? h89::Machine::kModem
                 : a == "--aux"     ? h89::Machine::kAux
                                    : -1;
        if (port >= 0 && i + 1 < argc) serialSpecs[port] = argv[++i];
        else positional.push_back(argv[i]);
    }
    std::string romDir = positional.empty() ? "roms" : positional[0];
    h89::RomScan scan = h89::scanRomDirectory(romDir);
    if (!scan.ok()) {
        showError(scan.error + "\n\n" + usage);
        return 1;
    }

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

    {
        App app;
        app.machine = std::make_unique<h89::Machine>(std::move(scan.config));
        app.machine->terminal()->onBell = ringBell;
        if (positional.size() > 1)
            insertDisks(app, static_cast<int>(positional.size() - 1), positional.data() + 1);
        for (int p = 0; p < h89::Machine::kSerialPorts; ++p) {
            std::string err;
            if (!serialSpecs[p].empty() && !app.serial[p].open(serialSpecs[p], &err)) showError(err);
        }
        app.machine->serialOutput = [&app](int port, uint8_t b) { app.serial[port].output(b); };
        app.machine->serialSource = [&app](int port) { return app.serial[port].next(); };
        if (!app.renderer.init()) {
            glfwTerminate();
            return 1;
        }
        app.window = window;
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
#endif

        // Fixed 60 Hz emulation, decoupled from the display refresh rate.
        constexpr double kFrameTime = 1.0 / h89::Machine::kFrameHz;
        double last = glfwGetTime();
        double pending = 0.0;

        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            for (int p = 0; p < h89::Machine::kSerialPorts; ++p) app.serial[p].pump(*app.machine, p);
            double now = glfwGetTime();
            pending += now - last;
            last = now;
            if (app.turbo) {
                // As many frames as fit in most of a display refresh.
                do app.debug.runFrame(*app.machine);
                while (glfwGetTime() - now < 0.012);
                pending = 0;
            }
            if (pending > 0.25) pending = 0.25; // don't spiral after a stall
            while (pending >= kFrameTime) {
                app.debug.runFrame(*app.machine);
                pending -= kFrameTime;
            }

            const auto& fb = app.machine->framebuffer();
            app.renderer.uploadFrame(fb.pixels.data(), h89::kScreenWidth, h89::kScreenHeight);
            int w, h;
            glfwGetFramebufferSize(window, &w, &h);
            app.renderer.draw(w, h);
#if H89_DEBUGGER
            if (app.debug.paused()) app.debuggerVisible = true; // e.g. a breakpoint was hit
            if (app.debuggerVisible) {
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();
                app.debuggerUI.draw(*app.machine, app.debug);
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
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
