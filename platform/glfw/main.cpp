// Windows (and Linux) front end: GLFW window + OpenGL 3.3 core.
//
//   H89-glfw [rom-folder [sy0.h8d [sy1.h8d [sy2.h8d]]]]
//
// The ROM folder defaults to "roms" in the working directory. Disk images
// can also be dropped on the window: they fill SY0:, SY1:, SY2: in order.

#include "GL.h" // must precede glfw3.h so GLFW doesn't pull in its own GL header
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "GLRenderer.h"
#include "Machine.h"
#include "RomSet.h"

#include <cstdio>
#include <memory>
#include <string>

namespace {

struct App {
    std::unique_ptr<h89::Machine> machine;
    h89::GLRenderer renderer;
    bool suppressNextChar = false; // keypad keys also generate a char event
};

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

void insertDisk(App& app, int drive, const char* path) {
    std::string err;
    if (!app.machine->h17()) {
        showError("This machine has no H17 controller (444-19 ROM missing).");
    } else if (!app.machine->h17()->insertDisk(drive, path, false, &err)) {
        showError(err);
    }
}

void onDrop(GLFWwindow* window, int count, const char** paths) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    for (int i = 0; i < count && i < h89::H17::kDrives; ++i) insertDisk(*app, i, paths[i]);
}

void onChar(GLFWwindow* window, unsigned int codepoint) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    if (app->suppressNextChar) {
        app->suppressNextChar = false;
        return;
    }
    if (codepoint >= 0x20 && codepoint < 0x7F) app->machine->terminal()->typeChar(static_cast<uint8_t>(codepoint));
}

} // namespace

int main(int argc, char** argv) {
    std::string romDir = argc > 1 ? argv[1] : "roms";
    h89::RomScan scan = h89::scanRomDirectory(romDir);
    if (!scan.ok()) {
        showError(scan.error + "\n\nUsage: H89-glfw [rom-folder [sy0.h8d [sy1.h8d [sy2.h8d]]]]");
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
        for (int d = 0; d < h89::H17::kDrives && d + 2 < argc; ++d) insertDisk(app, d, argv[d + 2]);
        if (!app.renderer.init()) {
            glfwTerminate();
            return 1;
        }
        glfwSetWindowUserPointer(window, &app);
        glfwSetKeyCallback(window, onKey);
        glfwSetCharCallback(window, onChar);
        glfwSetDropCallback(window, onDrop);

        // Fixed 60 Hz emulation, decoupled from the display refresh rate.
        constexpr double kFrameTime = 1.0 / h89::Machine::kFrameHz;
        double last = glfwGetTime();
        double pending = 0.0;

        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            double now = glfwGetTime();
            pending += now - last;
            last = now;
            if (pending > 0.25) pending = 0.25; // don't spiral after a stall
            while (pending >= kFrameTime) {
                app.machine->runFrame();
                pending -= kFrameTime;
            }

            const auto& fb = app.machine->framebuffer();
            app.renderer.uploadFrame(fb.pixels.data(), h89::kScreenWidth, h89::kScreenHeight);
            int w, h;
            glfwGetFramebufferSize(window, &w, &h);
            app.renderer.draw(w, h);
            glfwSwapBuffers(window);
        }
        app.renderer.shutdown();
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
