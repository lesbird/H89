#pragma once

#include "AppFiles.h"
#include "DebugControl.h"
#include "GLRenderer.h"
#include "Machine.h"
#include "SerialHost.h"

#if H89_DEBUGGER
#include "DebuggerUI.h"
#endif

#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

// State shared by the GLFW front end's main loop and its settings panel.
struct App {
    // Drives 0-2 are the H17's SY0:-SY2:, 3-6 the H37's DK0:-DK3:.
    static constexpr int kFirstH37Drive = h89::H17::kDrives;
    static constexpr int kDrives = h89::H17::kDrives + h89::H37::kDrives;

    h89::Settings settings;
    std::string builtInRoms;   // roms/ next to the program
    std::string bundledDisks;  // disks/ next to the program
    std::string disksDir;      // the user's copies
    std::string romOverride;   // a ROM folder given on the command line
    std::string romDir;        // the ROMs the machine was built from
    std::string romSummary;

    std::unique_ptr<h89::Machine> machine;
    h89::GLRenderer renderer;
    GLFWwindow* window = nullptr;
    bool suppressNextChar = false; // keypad keys also generate a char event
    int speed = 1;                 // frames per tick; 0 = as fast as possible
    int lastSpeed = 1;             // to return to from turbo
    int bell = 0;                  // 0 H19 speaker, 1 system sound, 2 off
    bool keyClick = true;
    std::vector<uint8_t> bellSound, clickSound;
    double lastBell = 0;
    h89::SerialHost serial[h89::Machine::kSerialPorts];
    h89::DebugControl debug;
#if H89_DEBUGGER
    h89::DebuggerUI debuggerUI;
    bool debuggerVisible = false;
    bool settingsVisible = false;
#endif
};

// Builds (or rebuilds) the machine from the settings and puts the saved
// disks back in the drives. Returns false, with `error`, if the ROMs fail.
bool buildMachine(App& app, std::string* error);
// Applies the settings that don't need a new machine: speed, display, sound,
// boot switches.
void applySettings(App& app);
void updateTitle(App& app);

std::string driveName(int drive);
bool driveAvailable(const App& app, int drive);
std::string diskPath(const App& app, int drive); // "" if empty
bool diskReadOnly(const App& app, int drive);
bool insertDisk(App& app, int drive, const std::string& path, bool readOnly);
void ejectDisk(App& app, int drive);
void restoreOriginalDisks(App& app);

bool connectSerial(App& app, int port, const std::string& spec);
void saveState(App& app, const std::string& path);
void loadState(App& app, const std::string& path);
std::string defaultStatePath();

void showError(const std::string& message);
// Native pickers (Windows); "" if cancelled or unavailable.
std::string chooseOpenFile(const char* title, const char* filter);
std::string chooseSaveFile(const char* title, const char* filter, const char* defaultName);
std::string chooseFolder(const char* title);
void openFolder(const std::string& path);

#if H89_DEBUGGER
// The settings panel (Ctrl+Alt+P).
void drawSettings(App& app);
#endif
