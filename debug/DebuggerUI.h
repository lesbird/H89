#pragma once

#include "DebugControl.h"

#include <cstdint>

namespace h89 {

// The debugger's Dear ImGui panels: run controls, CPU registers,
// disassembly with breakpoints, a memory view and the state of the I/O
// devices. Call draw() between ImGui::NewFrame() and ImGui::Render(); the
// front end owns the ImGui context and backends.
class DebuggerUI {
public:
    // `fillViewport` makes the panel the whole (host) window instead of a
    // floating ImGui window over the screen.
    void draw(Machine& m, DebugControl& control, bool fillViewport = false);

private:
    void drawControls(Machine& m, DebugControl& control);
    void drawRegisters(Machine& m, bool editable);
    void drawDisassembly(Machine& m, DebugControl& control);
    void drawMemory(Machine& m, bool editable);
    void drawBreakpoints(Machine& m);
    void drawDevices(Machine& m);

    uint16_t disasmStart_ = 0;
    bool followPc_ = true;
    uint16_t memoryAddress_ = 0x2000;
    uint16_t newBreakpoint_ = 0;
    uint16_t gotoAddress_ = 0;
};

} // namespace h89
