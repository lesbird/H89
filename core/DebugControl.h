#pragma once

#include "Machine.h"

namespace h89 {

// Run control for a debugger: pause, single step, step over, and stopping
// at the machine's breakpoints. Front ends call runFrame() instead of
// Machine::runFrame() each video frame.
class DebugControl {
public:
    bool paused() const { return paused_; }
    void pause() { paused_ = true; }
    void resume() { paused_ = false; }

    // Runs one video frame unless paused; pauses at a breakpoint.
    void runFrame(Machine& m);
    // Executes one instruction and stays paused.
    void step(Machine& m);
    // Like step(), but runs a CALL, RST, DJNZ or block instruction (LDIR,
    // ...) to completion, stopping at the instruction after it.
    void stepOver(Machine& m);

private:
    void clearTemporary(Machine& m);

    bool paused_ = false;
    int temporary_ = -1;      // step-over breakpoint address, or -1
    bool temporaryWasSet_ = false; // a user breakpoint was already there
};

} // namespace h89
