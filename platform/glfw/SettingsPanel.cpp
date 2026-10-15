// The GLFW front end's settings panel (Ctrl+Alt+P): machine, speed,
// display, sound, drives, serial ports and saved states, plus the keyboard
// shortcuts, since this front end has no menu bar.

#include "App.h"

#include "imgui.h"

#include <algorithm>
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
    if (terminalOnly(app)) {
        note("Running as an H19 terminal on its own: see the Terminal tab. The ROMs below still supply "
             "its firmware and font.");
    }
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
    if (ImGui::Button("Reset")) {
        if (terminalOnly(app)) h19(app).reset();
        else app.machine->reset();
    }
}

h89::TextSendOptions textOptions(const App& app) {
    using LE = h89::TextSendOptions::LineEnding;
    h89::TextSendOptions o;
    int ending = app.settings.getInt("textLineEnding", 0);
    o.lineEnding = ending == 1 ? LE::CRLF : ending == 2 ? LE::LF : ending == 3 ? LE::AsIs : LE::CR;
    o.charDelayMs = std::clamp(app.settings.getInt("textCharDelay", 0), 0, 5000);
    o.lineDelayMs = std::clamp(app.settings.getInt("textLineDelay", 0), 0, 10000);
    return o;
}

void startTransfer(App& app, std::unique_ptr<h89::FileTransfer> transfer, const std::string& error) {
    if (!transfer) showError(error);
    else if (!app.link->startTransfer(std::move(transfer))) showError("A transfer is already running.");
}

void transferSection(App& app) {
    ImGui::SeparatorText("File transfer");
    if (!terminalOnly(app)) {
        ImGui::TextDisabled("For the terminal on a serial port.");
        return;
    }
    // Windows has file pickers; elsewhere the path is typed in.
    static char typed[512] = "";
#if defined(_WIN32)
    const bool pickers = true;
#else
    const bool pickers = false;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##xferpath", "File (or folder, for ZMODEM downloads) path", typed, sizeof typed);
#endif
    const char* any = "All files\0*.*\0";
    const bool busy = app.link->transferRunning();
    ImGui::BeginDisabled(busy);
    std::string error;
    if (ImGui::Button("Send Text File...")) {
        std::string path = pickers ? chooseOpenFile("Choose a text file to type to the computer", any) : typed;
        if (!path.empty()) startTransfer(app, h89::sendTextFile(path, textOptions(app), &error), error);
    }
    ImGui::Text("Upload:");
    for (int k = 0; k < 3; ++k) {
        static const char* const names[] = {"XMODEM...", "XMODEM-1K...", "ZMODEM..."};
        ImGui::SameLine();
        if (!ImGui::Button(names[k])) continue;
        std::vector<std::string> paths;
        if (!pickers) paths = {typed};
        else if (k == 2) paths = chooseOpenFiles("Choose files to send with ZMODEM", any);
        else paths = {chooseOpenFile("Choose a file to send with XMODEM", any)};
        if (paths.empty() || paths[0].empty()) continue;
        startTransfer(app, k == 2 ? h89::zmodemSend(paths, &error) : h89::xmodemSend(paths[0], k == 1, &error), error);
    }
    ImGui::Text("Download:");
    ImGui::SameLine();
    if (ImGui::Button("XMODEM... ")) {
        std::string path = pickers ? chooseSaveFile("Save the file from the computer (XMODEM doesn't send its name)",
                                                    any, "DOWNLOAD.BIN")
                                   : typed;
        if (!path.empty()) startTransfer(app, h89::xmodemReceive(path, &error), error);
    }
    ImGui::SameLine();
    if (ImGui::Button("ZMODEM")) startTransfer(app, h89::zmodemReceive(), "");
    ImGui::EndDisabled();
    note("Start the computer's receive or send program before or after these. ZMODEM downloads ask where to "
         "save each file the computer sends.");

    ImGui::Text("Text files:");
    ImGui::SetNextItemWidth(160);
    settingCombo(app, "Line endings", "textLineEnding", 0,
                 {{"CR (Return)", 0}, {"CR LF", 1}, {"LF", 2}, {"Unchanged", 3}});
    for (const auto& [label, key] : {std::pair{"ms after each character", "textCharDelay"},
                                     std::pair{"ms after each line", "textLineDelay"}}) {
        int ms = app.settings.getInt(key, 0);
        ImGui::SetNextItemWidth(100);
        if (ImGui::InputInt(label, &ms, 1, 10)) {
            app.settings.setInt(key, std::clamp(ms, 0, 10000));
            app.settings.save();
        }
    }
}

void terminalSection(App& app) {
    h89::Settings& s = app.settings;
    int mode = terminalOnly(app) ? 1 : 0;
    ImGui::SetNextItemWidth(260);
    if (choice("Run as", mode, {{"H89 computer", 0}, {"H19 terminal on a serial port", 1}})) {
        app.terminalOverride.clear();
        s.setBool("terminalOnly", mode == 1);
        s.save();
        std::string error;
        if (!buildMachine(app, &error)) showError(error);
    }
    note("As a terminal, H89 is just the H19, talking to a real computer (an H8, say) through a serial "
         "port such as a USB serial adapter.");
    if (!app.terminalOverride.empty()) {
        ImGui::Text("Using %s from the command line.", app.terminalOverride.c_str());
        if (terminalOnly(app)) ImGui::TextWrapped("Status: %s", app.link->description().c_str());
        transferSection(app);
        return;
    }
    ImGui::SeparatorText("Serial port");

    bool changed = false;
    static char port[256];
    static bool loaded = false;
    if (!loaded) {
        std::snprintf(port, sizeof port, "%s", s.get("terminalPort").c_str());
        loaded = true;
    }
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("Port", port[0] ? port : "choose...")) {
        for (const auto& p : h89::SerialPort::list()) {
            if (ImGui::Selectable(p.c_str(), p == port)) {
                std::snprintf(port, sizeof port, "%s", p.c_str());
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    if (ImGui::InputTextWithHint("##portname", "or type its name", port, sizeof port,
                                 ImGuiInputTextFlags_EnterReturnsTrue))
        changed = true;

    int baud = s.getInt("terminalBaud", 9600);
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("Speed", std::to_string(baud).c_str())) {
        for (size_t i = 0; i < h89::kCommonBaudRateCount; ++i) {
            int b = h89::kCommonBaudRates[i];
            if (ImGui::Selectable(std::to_string(b).c_str(), b == baud)) {
                baud = b;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    static int typedBaud = 0;
    if (!typedBaud) typedBaud = baud;
    ImGui::SetNextItemWidth(160);
    if (ImGui::InputInt("##baud", &typedBaud, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue) && typedBaud >= 50) {
        baud = typedBaud;
        changed = true;
    }
    note("Any rate the adapter supports: pick one or type it and press Enter. The H19's own baud rate "
         "switches don't apply.");

    static const char* const formats[] = {"8N1", "8N2", "8E1", "8O1", "7E1", "7O1", "7N2"};
    std::string format = s.get("terminalFormat", "8N1");
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("Format", format.c_str())) {
        for (const char* f : formats) {
            if (ImGui::Selectable(f, format == f)) {
                format = f;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    bool rtsCts = s.getBool("terminalRtsCts", false);
    if (ImGui::Checkbox("RTS/CTS hardware flow control", &rtsCts)) changed = true;

    if (changed) {
        typedBaud = baud;
        s.set("terminalPort", port);
        s.setInt("terminalBaud", baud);
        s.set("terminalFormat", format);
        s.setBool("terminalRtsCts", rtsCts);
        s.save();
        connectTerminal(app);
    }
    if (terminalOnly(app)) ImGui::TextWrapped("Status: %s", app.link->description().c_str());
    else ImGui::TextDisabled("Status: not in use (running as an H89)");

    transferSection(app);

    ImGui::SeparatorText("H19 switches");
    for (int i = 0; i < kTerminalSwitchCount; ++i)
        settingCheckbox(app, kTerminalSwitches[i].title, kTerminalSwitches[i].key, kTerminalSwitches[i].fallback);
    note("Switch changes reset the terminal.");
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

void drawTransfer(App& app) {
    h89::FileTransfer* t = app.link->transfer();
    if (t->state() == h89::FileTransfer::State::NeedsSavePath) {
        // A ZMODEM download named a file: ask where it goes ("" skips it).
#if defined(_WIN32)
        std::string where = chooseSaveFile(("Save " + t->offeredName() + " from the computer").c_str(),
                                           "All files\0*.*\0", t->offeredName().c_str());
#else
        std::string where = t->offeredName(); // the working folder
#endif
        t->setSavePath(where);
    }
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2, 60), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0));
    ImGui::Begin("File Transfer", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(t->title().c_str());
    float fraction = t->bytesTotal() ? static_cast<float>(t->bytesDone()) / static_cast<float>(t->bytesTotal()) : 0.f;
    if (t->state() == h89::FileTransfer::State::Done) fraction = 1.f;
    ImGui::ProgressBar(fraction, ImVec2(-1, 0), "");
    ImGui::TextWrapped("%s", t->status().c_str());
    if (app.link->transferRunning()) {
        if (ImGui::Button("Cancel")) app.link->cancelTransfer();
    } else if (ImGui::Button("Close")) {
        app.link->clearTransfer();
    }
    ImGui::End();
}

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
        if (ImGui::BeginTabItem("Terminal")) {
            terminalSection(app);
            ImGui::EndTabItem();
        }
        // The rest belong to the computer, not a terminal on its own.
        const bool computer = !terminalOnly(app);
        if (computer && ImGui::BeginTabItem("Disks")) {
            disksSection(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Display & Sound")) {
            displaySection(app);
            ImGui::EndTabItem();
        }
        if (computer && ImGui::BeginTabItem("Serial Ports")) {
            serialSection(app);
            ImGui::EndTabItem();
        }
        if (computer && ImGui::BeginTabItem("States")) {
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
