// Minimal CP/M environment for running the ZEXDOC / ZEXALL instruction
// exercisers (or any .COM that only uses BDOS functions 2 and 9).
//
//   zexrun <file.com>
//
// Exits 0 if the program finished and printed no "ERROR".

#include "cpu/Z80.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

class CpmBus : public h89::Z80Bus {
public:
    std::array<uint8_t, 65536> mem{};
    uint8_t memRead(uint16_t a) override { return mem[a]; }
    void memWrite(uint16_t a, uint8_t v) override { mem[a] = v; }
    uint8_t ioRead(uint16_t) override { return 0xFF; }
    void ioWrite(uint16_t, uint8_t) override {}
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <file.com>\n", argv[0]);
        return 2;
    }

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(in)), {});

    CpmBus bus;
    std::copy(image.begin(), image.end(), bus.mem.begin() + 0x100);
    // BDOS entry at 0005 jumps to the top of the TPA; (0006) is read by the
    // exercisers to place their stack. We trap PC == 0005 before it executes.
    bus.mem[0x0005] = 0xC3;
    bus.mem[0x0006] = 0x00;
    bus.mem[0x0007] = 0xF0;
    bus.mem[0xF000] = 0xC9;

    h89::Z80 cpu(bus);
    auto& r = cpu.regs();
    r.pc = 0x0100;
    r.sp = 0xF000;

    std::string output;
    auto start = std::chrono::steady_clock::now();

    for (;;) {
        if (r.pc == 0x0000) break; // warm boot: program finished
        if (r.pc == 0x0005) {
            switch (r.bc.l) {
            case 2:
                output += static_cast<char>(r.de.l);
                std::putchar(r.de.l);
                break;
            case 9:
                for (uint16_t a = r.de.w; bus.mem[a] != '$'; ++a) {
                    output += static_cast<char>(bus.mem[a]);
                    std::putchar(bus.mem[a]);
                }
                break;
            default:
                break;
            }
            std::fflush(stdout);
            // Return to caller
            r.pc = bus.mem[r.sp] | (bus.mem[r.sp + 1] << 8);
            r.sp += 2;
            continue;
        }
        cpu.step();
    }

    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("\n%llu T-states in %.1fs (%.0f MHz effective)\n",
                static_cast<unsigned long long>(cpu.cycles()), secs,
                cpu.cycles() / secs / 1e6);

    return output.find("ERROR") == std::string::npos ? 0 : 1;
}
