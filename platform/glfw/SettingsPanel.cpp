// The GLFW front end's settings panel (Ctrl+Alt+P): machine, speed,
// display, sound, drives, serial ports and saved states, plus the keyboard
// shortcuts, since this front end has no menu bar.

#include "App.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

const char* const kDiskFilter = "Disk images (*.h8d;*.h37;*.imd)\0*.h8d;*.h37;*.imd\0All files\0*.*\0";
const char* const kStateFilter = "H89 states (*.h89state)\0*.h89state\0All files\0*.*\0";

// A combo over (label, value) pairs; true if the user picked a new value.
bool choice(const char* label, int& value, std::initializer_list<std::pair<const char*, int>> options) {
    const char* current = options.begin()->first;
    for (const auto& [name, v] : options)
        if (v == value) current = name;
    bool changed = false;
    if (ImGui::BeginCombo(label, current)) {
        for (const auto& [name, v] : options) {
            if (ImGui::Selectable(name, v == value) && v != value) {
                value = v;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void settingCombo(App& app, const char* label, const char* key, int fallback,
                  std::initializer_list<std::pair<const char*, int>> options, bool rebuild = false) {
    int value = app.settings.getInt(key, fallback);
    if (!choice(label, value, options)) return;
    app.settings.setInt(key, value);
    app.settings.save();
    if (rebuild) {
        std::string error;
        if (!buildMachine(app, &error)) showError(error);
    } else {
        applySettings(app);
    }
}

void settingCheckbox(App& app, const char* label, const char* key, bool fallback) {
    bool value = app.settings.getBool(key, fallback);
    if (!ImGui::Checkbox(label, &value)) return;
    app.settings.setBool(key, value);
    app.settings.save();
    applySettings(app);
}

void note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void machineSection(App& app) {
    std::string roms = app.settings.get("roms");
    ImGui::Text("ROMs:");
    ImGui::SameLine();
    if (!app.romOverride.empty()) ImGui::TextWrapped("%s (from the command line)", app.romOverride.c_str());
    else if (roms.empty()) ImGui::TextUnformatted("the ones that came with H89");
    else ImGui::TextWrapped("%s", roms.c_str());
    if (ImGui::Button("Choose ROM Folder...")) {
        std::string dir = chooseFolder("Choose a folder of H89 and H19 ROM images, named by part number");
        if (!dir.empty()) {
            app.romOverride.clear();
            app.settings.set("roms", dir);
            app.settings.save();
            std::string error;
            if (!buildMachine(app, &error)) showError(error);
        }
    }
    if (!roms.empty() || !app.romOverride.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Use Built-in ROMs")) {
            app.romOverride.clear();
            app.settings.remove("roms");
            app.settings.save();
            std::string error;
            if (!buildMachine(app, &error)) showError(error);
        }
    }
    ImGui::SetNextItemWidth(220);
    settingCombo(app, "Monitor", "monitor", 90, {{"MTR-90 (444-142)", 90}, {"MTR-89 (444-62)", 89}}, true);
    ImGui::SetNextItemWidth(220);
    settingCombo(app, "Memory", "memory", 64, {{"16K", 16}, {"32K", 32}, {"48K", 48}, {"64K", 64}}, true);
    note("Changing the ROMs, monitor or memory restarts the H89.");
    ImGui::SetNextItemWidth(220);
    settingCombo(app, "Boot from", "bootH37", 0, {{"H17 hard-sectored (SY0:)", 0}, {"H37 soft-sectored (DK0:)", 1}});
    settingCheckbox(app, "Boot from the disk at power-on and reset", "autoBoot", false);
    ImGui::SetNextItemWidth(220);
    settingCombo(app, "Speed", "speed", 1, {{"Normal (2 MHz)", 1}, {"2x", 2}, {"4x", 4}, {"Maximum", 0}});
    if (ImGui::Button("Reset")) app.machine->reset();
}

void displaySection(App& app) {
    ImGui::SetNextItemWidth(220);
    settingCombo(app, "Phosphor", "phosphor", 0, {{"White (P4)", 0}, {"Green (P1)", 1}, {"Amber (P3)", 2}});
    settingCheckbox(app, "CRT effects (glow, persistence, curved screen)", "crt", true);
    ImGui::SetNextItemWidth(220);
    settingCombo(app, "Bell", "bell", 0, {{"H19 speaker", 0}, {"System sound", 1}, {"Off", 2}});
    settingCheckbox(app, "Click when a key is pressed", "keyClick", true);
}

void disksSection(App& app) {
    // Windows has a file picker; elsewhere the path is typed in.
    static char typedPath[512] = "";
#if defined(_WIN32)
    const bool pickers = true;
#else
    const bool pickers = false;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##path", "Disk image path, then Insert", typedPath, sizeof typedPath);
#endif
    if (ImGui::BeginTable("drives", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Drive");
        ImGui::TableSetupColumn("Disk", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("");
        for (int d = 0; d < App::kDrives; ++d) {
            ImGui::PushID(d);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(driveName(d).c_str());
            ImGui::TableNextColumn();
            if (!driveAvailable(app, d)) {
                ImGui::TextDisabled(d < App::kFirstH37Drive ? "no H17 controller (444-19 ROM missing)"
                                                            : "no H37 controller");
                ImGui::TableNextColumn();
                ImGui::PopID();
                continue;
            }
            std::string path = diskPath(app, d);
            if (path.empty()) {
                ImGui::TextDisabled("empty");
            } else {
                ImGui::TextUnformatted(fs::path(path).filename().string().c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
                if (diskReadOnly(app, d)) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(write protected)");
                }
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Insert")) {
                std::string chosen = pickers ? chooseOpenFile(d < App::kFirstH37Drive ? "Insert an H17 disk image (.h8d)"
                                                                                      : "Insert an H37 disk image (.h37, .imd)",
                                                              kDiskFilter)
                                             : std::string(typedPath);
                if (!chosen.empty()) insertDisk(app, d, chosen, false);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Protect")) {
                if (!path.empty()) insertDisk(app, d, path, !diskReadOnly(app, d));
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle write protection");
            ImGui::SameLine();
            ImGui::BeginDisabled(path.empty());
            if (ImGui::SmallButton("Eject")) ejectDisk(app, d);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    note("You can also drop disk images on the window. Writes go straight back to the image file.");
    ImGui::Text("Your disks: %s", app.disksDir.c_str());
    if (ImGui::Button("Open Disks Folder")) openFolder(app.disksDir);
    if (!app.bundledDisks.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Restore Original Disks...")) ImGui::OpenPopup("Restore the original disks?");
    }
    if (ImGui::BeginPopupModal("Restore the original disks?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Your copies of the disks that came with H89 are replaced by fresh ones,\n"
                               "losing any changes made to them. Other disks in the folder are not touched.");
        if (ImGui::Button("Restore")) {
            restoreOriginalDisks(app);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void serialSection(App& app) {
    static const char* const names[] = {"AUX (D0h)", "MODEM (D8h)", "LP: printer (E0h)"};
    static char specs[h89::Machine::kSerialPorts][256];
    static bool loaded = false;
    if (!loaded) {
        for (int p = 0; p < h89::Machine::kSerialPorts; ++p)
            std::snprintf(specs[p], sizeof specs[p], "%s", app.serial[p].active() ? app.serial[p].spec().c_str() : "");
        loaded = true;
    }
    note("A file path (output is added to its end), listen:PORT for a TCP client such as "
         "\"telnet localhost 8089\", or connect:HOST:PORT. Leave empty to disconnect.");
    for (int p = 0; p < h89::Machine::kSerialPorts; ++p) {
        ImGui::PushID(p);
        ImGui::SetNextItemWidth(260);
        bool enter = ImGui::InputText(names[p], specs[p], sizeof specs[p], ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Connect") || enter) connectSerial(app, p, specs[p]);
        ImGui::TextDisabled("    %s", app.serial[p].description().c_str());
        ImGui::PopID();
    }
}

void stateSection(App& app) {
    if (ImGui::Button("Save State...")) {
        std::string path = chooseSaveFile("Save the machine's state", kStateFilter, "H89.h89state");
#if !defined(_WIN32)
        path = defaultStatePath();
#endif
        if (!path.empty()) saveState(app, path);
    }
    ImGui::SameLine();
    if (ImGui::Button("Load State...")) {
        std::string path = chooseOpenFile("Load a saved machine state", kStateFilter);
#if !defined(_WIN32)
        path = defaultStatePath();
#endif
        if (!path.empty()) loadState(app, path);
    }
    note("A state holds memory, both Z80s, the terminal and which disks are in the drives; the disks "
         "themselves stay in their files. Ctrl+Alt+S and Ctrl+Alt+L save and load a quick state:");
    note(defaultStatePath().c_str());
}

void keysSection() {
    static const char* const rows[][2] = {
        {"Ctrl+Alt+P", "Show or hide these settings"},
        {"Ctrl+Alt+R", "Reset"},
        {"Ctrl+Alt+T", "Turbo (maximum speed) on or off"},
        {"Ctrl+Alt+S / L", "Save / load the quick state"},
        {"Ctrl+Alt+1 / 2 / 3", "White, green, amber phosphor"},
        {"Ctrl+Alt+C", "CRT effects on or off"},
        {"Ctrl+Alt+D", "Debugger"},
        {"F1-F5", "H19 F1-F5"},
        {"F6 F7 F8 F9", "ERASE, BLUE, RED, WHITE"},
        {"F10 (held)", "BREAK"},
        {"Page Down", "SCROLL"},
        {"Arrows, Home", "Cursor keys (shifted keypad)"},
        {"Numeric keypad", "H19 keypad"},
    };
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (const auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r[0]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r[1]);
        }
        ImGui::EndTable();
    }
    note("To start an operating system, type B and Return at the H: prompt.");
}

} // namespace

void drawSettings(App& app) {
    ImGui::SetNextWindowSize(ImVec2(620, 560), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("H89 Settings", &app.settingsVisible)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("settings")) {
        if (ImGui::BeginTabItem("Machine")) {
            machineSection(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Disks")) {
            disksSection(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Display & Sound")) {
            displaySection(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Serial Ports")) {
            serialSection(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("States")) {
            stateSection(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Keys")) {
            keysSection();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
