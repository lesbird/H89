#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace h89 {

struct Disassembly {
    std::string text; // e.g. "LD A,(IX+05h)"
    int length = 1;   // bytes consumed
};

// Disassembles the instruction at `pc`, reading bytes through `read`.
Disassembly disassembleZ80(const std::function<uint8_t(uint16_t)>& read, uint16_t pc);

} // namespace h89
