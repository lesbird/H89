// Disassembles a raw binary: z80dis <file> [origin-hex] [start-hex] [count]

#include "cpu/Z80Disasm.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file> [origin-hex] [start-hex] [count]\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
    unsigned origin = argc > 2 ? std::strtoul(argv[2], nullptr, 16) : 0;
    unsigned pc = argc > 3 ? std::strtoul(argv[3], nullptr, 16) : origin;
    long count = argc > 4 ? std::strtol(argv[4], nullptr, 0) : -1;

    auto read = [&](uint16_t a) -> uint8_t {
        unsigned off = static_cast<uint16_t>(a - origin);
        return off < data.size() ? data[off] : 0xFF;
    };

    while (pc < origin + data.size() && count-- != 0) {
        auto d = h89::disassembleZ80(read, static_cast<uint16_t>(pc));
        std::printf("%04X  ", pc);
        for (int i = 0; i < 4; ++i) {
            if (i < d.length) std::printf("%02X ", read(static_cast<uint16_t>(pc + i)));
            else std::printf("   ");
        }
        std::printf(" %s\n", d.text.c_str());
        pc += d.length;
    }
    return 0;
}
