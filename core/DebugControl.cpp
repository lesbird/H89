#include "DebugControl.h"

#include "cpu/Z80Disasm.h"

namespace h89 {

void DebugControl::runFrame(Machine& m) {
    if (paused_) return;
    m.runFrame();
    if (m.stopped()) {
        paused_ = true;
        clearTemporary(m);
    }
}

void DebugControl::step(Machine& m) {
    clearTemporary(m);
    m.stepInstruction();
    paused_ = true;
}

void DebugControl::stepOver(Machine& m) {
    uint16_t pc = m.cpu().regs().pc;
    uint8_t op = m.peek(pc), op2 = m.peek(static_cast<uint16_t>(pc + 1));
    bool call = op == 0xCD || (op & 0xC7) == 0xC4; // CALL, CALL cc
    bool rst = (op & 0xC7) == 0xC7;
    bool djnz = op == 0x10;
    bool block = op == 0xED && (op2 & 0xF4) == 0xB0; // LDIR, CPIR, INIR, OTIR and the D forms
    if (!call && !rst && !djnz && !block) {
        step(m);
        return;
    }
    clearTemporary(m);
    int length = disassembleZ80([&](uint16_t a) { return m.peek(a); }, pc).length;
    temporary_ = static_cast<uint16_t>(pc + length);
    temporaryWasSet_ = m.hasBreakpoint(static_cast<uint16_t>(temporary_));
    m.setBreakpoint(static_cast<uint16_t>(temporary_), true);
    m.stepInstruction(); // off the current instruction, even if it has a breakpoint
    paused_ = false;
}

void DebugControl::clearTemporary(Machine& m) {
    if (temporary_ >= 0 && !temporaryWasSet_) m.setBreakpoint(static_cast<uint16_t>(temporary_), false);
    temporary_ = -1;
}

} // namespace h89
