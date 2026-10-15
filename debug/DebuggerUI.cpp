#include "DebuggerUI.h"

#include "cpu/Z80Disasm.h"

#include "imgui.h"

#include <cctype>
#include <cstdio>
#include <string>

namespace h89 {

namespace {

constexpr int kDisasmLines = 24;
constexpr int kMemoryRows = 16;

bool hex16(const char* label, uint16_t* value, bool editable) {
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("FFFF").x + ImGui::GetStyle().FramePadding.x * 2);
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase;
    if (!editable) flags |= ImGuiInputTextFlags_ReadOnly;
    return ImGui::InputScalar(label, ImGuiDataType_U16, value, nullptr, nullptr, "%04X", flags);
}

bool hex8(const char* label, uint8_t* value, bool editable) {
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("FF").x + ImGui::GetStyle().FramePadding.x * 2);
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase;
    if (!editable) flags |= ImGuiInputTextFlags_ReadOnly;
    return ImGui::InputScalar(label, ImGuiDataType_U8, value, nullptr, nullptr, "%02X", flags);
}

} // namespace

void DebuggerUI::draw(Machine& m, DebugControl& control, bool fillViewport) {
    ImGuiWindowFlags flags = 0;
    if (fillViewport) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;
    } else {
        ImGui::SetNextWindowSize(ImVec2(760, 640), ImGuiCond_FirstUseEver);
    }
    ImGui::Begin("H89 Debugger", nullptr, flags);
    drawControls(m, control);
    ImGui::Separator();

    bool editable = control.paused();
    if (ImGui::BeginTable("layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthFixed, 250);
        ImGui::TableNextColumn();
        drawRegisters(m, editable);
        ImGui::Spacing();
        drawBreakpoints(m);
        ImGui::TableNextColumn();
        drawDisassembly(m, control);
        ImGui::EndTable();
    }
    ImGui::Separator();
    if (ImGui::BeginTabBar("lower")) {
        if (ImGui::BeginTabItem("Memory")) {
            drawMemory(m, editable);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Devices")) {
            drawDevices(m);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void DebuggerUI::drawControls(Machine& m, DebugControl& control) {
    if (control.paused()) {
        if (ImGui::Button("Run")) control.resume();
    } else {
        if (ImGui::Button("Pause")) control.pause();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!control.paused());
    if (ImGui::Button("Step")) {
        control.step(m);
        followPc_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Step Over")) {
        control.stepOver(m);
        followPc_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reset")) m.reset();
    ImGui::SameLine();
    const Z80& cpu = m.cpu();
    if (control.paused()) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Paused at %04X%s", cpu.regs().pc,
                           m.hasBreakpoint(cpu.regs().pc) ? " (breakpoint)" : "");
    } else {
        ImGui::TextDisabled("Running");
    }
}

void DebuggerUI::drawRegisters(Machine& m, bool editable) {
    Z80Regs& r = m.cpu().regs();
    ImGui::TextUnformatted("CPU board Z80");
    if (ImGui::BeginTable("regs", 4, ImGuiTableFlags_SizingFixedFit)) {
        auto pair = [&](const char* name, RegPair& p, const char* alt, RegPair& q) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            ImGui::TableNextColumn();
            hex16((std::string("##") + name).c_str(), &p.w, editable);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(alt);
            ImGui::TableNextColumn();
            hex16((std::string("##") + alt).c_str(), &q.w, editable);
        };
        pair("AF", r.af, "AF'", r.af2);
        pair("BC", r.bc, "BC'", r.bc2);
        pair("DE", r.de, "DE'", r.de2);
        pair("HL", r.hl, "HL'", r.hl2);
        pair("IX", r.ix, "IY", r.iy);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("SP");
        ImGui::TableNextColumn();
        hex16("##SP", &r.sp, editable);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("PC");
        ImGui::TableNextColumn();
        if (hex16("##PC", &r.pc, editable)) followPc_ = true;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("I");
        ImGui::TableNextColumn();
        hex8("##I", &r.i, editable);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("R");
        ImGui::TableNextColumn();
        hex8("##R", &r.r, editable);
        ImGui::EndTable();
    }
    uint8_t f = r.af.l;
    const char* names = "SZYHXPNC";
    char flags[9];
    for (int i = 0; i < 8; ++i) flags[i] = (f & (0x80 >> i)) ? names[i] : '-';
    flags[8] = 0;
    ImGui::Text("Flags %s   IM %d   IFF %d%d", flags, r.im, r.iff1, r.iff2);
    ImGui::Text("%s  cycles %llu", m.cpu().halted() ? "HALTED" : "running",
                static_cast<unsigned long long>(m.cpu().cycles()));
}

void DebuggerUI::drawDisassembly(Machine& m, DebugControl& control) {
    uint16_t pc = m.cpu().regs().pc;
    auto read = [&](uint16_t a) { return m.peek(a); };

    // Keep the PC in view while it is followed.
    if (followPc_ && control.paused()) {
        uint16_t a = disasmStart_;
        bool visible = false;
        for (int i = 0; i < kDisasmLines - 4; ++i) {
            if (a == pc) visible = true;
            a = static_cast<uint16_t>(a + disassembleZ80(read, a).length);
        }
        if (!visible) disasmStart_ = pc;
    }
    if (!control.paused()) disasmStart_ = pc;

    ImGui::TextUnformatted("Disassembly");
    ImGui::SameLine();
    if (hex16("Go to", &gotoAddress_, true)) {
        disasmStart_ = gotoAddress_;
        followPc_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Follow PC", &followPc_) && followPc_) disasmStart_ = pc;

    ImGui::TextDisabled("Click an address to toggle a breakpoint.");
    uint16_t a = disasmStart_;
    for (int i = 0; i < kDisasmLines; ++i) {
        Disassembly d = disassembleZ80(read, a);
        char bytes[16] = {};
        for (int k = 0; k < d.length && k < 4; ++k)
            std::snprintf(bytes + k * 3, sizeof bytes - k * 3, "%02X ", m.peek(static_cast<uint16_t>(a + k)));
        bool bp = m.hasBreakpoint(a);
        char label[32];
        std::snprintf(label, sizeof label, "%s %04X##%d", bp ? "*" : " ", a, i);
        ImVec4 colour = a == pc ? ImVec4(1, 0.85f, 0.3f, 1) : bp ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(0.8f, 0.8f, 0.8f, 1);
        ImGui::PushStyleColor(ImGuiCol_Text, colour);
        if (ImGui::Selectable(label, a == pc, 0, ImVec2(ImGui::CalcTextSize("* FFFF").x, 0))) m.setBreakpoint(a, !bp);
        ImGui::SameLine();
        ImGui::Text("%-12s %s", bytes, d.text.c_str());
        ImGui::PopStyleColor();
        a = static_cast<uint16_t>(a + d.length);
    }
}

void DebuggerUI::drawMemory(Machine& m, bool editable) {
    hex16("Address", &memoryAddress_, true);
    ImGui::SameLine();
    ImGui::TextDisabled(editable ? "(click a byte to change it)" : "(pause to edit)");
    uint16_t base = static_cast<uint16_t>(memoryAddress_ & 0xFFF0);
    for (int row = 0; row < kMemoryRows; ++row) {
        uint16_t addr = static_cast<uint16_t>(base + row * 16);
        ImGui::Text("%04X ", addr);
        char ascii[17] = {};
        for (int col = 0; col < 16; ++col) {
            uint16_t a = static_cast<uint16_t>(addr + col);
            uint8_t v = m.peek(a);
            ascii[col] = std::isprint(v & 0x7F) ? static_cast<char>(v & 0x7F) : '.';
            ImGui::SameLine(0, col == 8 ? 10 : 4);
            ImGui::PushID(a);
            if (editable) {
                if (hex8("##b", &v, true)) m.poke(a, v);
            } else {
                ImGui::Text("%02X", v);
            }
            ImGui::PopID();
        }
        ImGui::SameLine(0, 10);
        ImGui::TextUnformatted(ascii);
    }
}

void DebuggerUI::drawBreakpoints(Machine& m) {
    ImGui::TextUnformatted("Breakpoints");
    hex16("##newbp", &newBreakpoint_, true);
    ImGui::SameLine();
    if (ImGui::Button("Add")) m.setBreakpoint(newBreakpoint_, true);
    for (uint16_t a : m.breakpoints()) {
        ImGui::PushID(a);
        ImGui::Text("%04X", a);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) m.setBreakpoint(a, false);
        ImGui::SameLine();
        if (ImGui::SmallButton("Show")) {
            disasmStart_ = a;
            followPc_ = false;
        }
        ImGui::PopID();
    }
}

void DebuggerUI::drawDevices(Machine& m) {
    uint8_t gpp = m.gpp();
    ImGui::Text("GPP (F2h out) %02X: timer %s, ORG-0 %s, single step %s", gpp,
                (gpp & Machine::GPP_TIMER_ENABLE) ? "on" : "off", (gpp & Machine::GPP_ORG0) ? "on" : "off",
                (gpp & Machine::GPP_SINGLE_STEP) ? "on" : "off");
    ImGui::Text("SW501 (F2h in) %02X: boot from %s", m.config().sw501,
                (m.config().sw501 & sw501::kBootFrom170) ? "H37 (170Q)" : "H17 (174Q)");
    if (H17* h17 = m.h17()) {
        ImGui::Text("H17: motor %s, selected drive %d", h17->motorOn() ? "on" : "off", h17->selectedDrive());
        for (int d = 0; d < H17::kDrives; ++d) {
            const H17Disk& disk = h17->disk(d);
            ImGui::Text("  SY%d: track %2d  %s", d, h17->headTrack(d), disk.loaded() ? disk.path().c_str() : "(empty)");
        }
    }
    if (H37* h37 = m.h37()) {
        ImGui::Text("H37: %s", h37->busy() ? "busy" : "idle");
        for (int d = 0; d < H37::kDrives; ++d) {
            const SoftDisk& disk = h37->disk(d);
            ImGui::Text("  DK%d: cylinder %2d  %s", d, h37->headCylinder(d),
                        disk.loaded() ? disk.path().c_str() : "(empty)");
        }
    }
    if (H19* t = m.terminal()) ImGui::Text("H19 terminal: frame %llu", static_cast<unsigned long long>(t->frameCount()));
}

} // namespace h89
