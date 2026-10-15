#include "Machine.h"
#include "check.h"
#include "devices/Ins8250.h"

#include <initializer_list>
#include <string>

using h89::Ins8250;
using h89::Machine;
using h89::MachineConfig;

namespace {

// Builds a 4K monitor ROM image with code placed at the given addresses.
struct RomBuilder {
    std::vector<uint8_t> rom = std::vector<uint8_t>(4096, 0x00);
    RomBuilder& at(uint16_t addr, std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) rom[addr++] = b;
        return *this;
    }
};

uint16_t read16(Machine& m, uint16_t addr) { return m.peek(addr) | (m.peek(addr + 1) << 8); }

} // namespace

TEST_CASE("8250 transmits at the programmed baud rate") {
    Ins8250 uart(Machine::kCpuHz);
    std::string sent;
    uart.onTransmit = [&](uint8_t b) { sent += static_cast<char>(b); };
    uart.write(3, 0x80); // DLAB
    uart.write(0, 12);   // 9600 baud
    uart.write(1, 0);
    uart.write(3, 0x03); // 8N1

    uart.write(0, 'A');
    CHECK_EQ(uart.read(5) & 0x60, 0x20); // THRE set (moved to shift register), TEMT clear
    uart.write(0, 'B');
    CHECK_EQ(uart.read(5) & 0x20, 0x00); // holding register now full

    // 10 bits x 16 x 12 / 1.8432 MHz at 2.048 MHz CPU = 2133 cycles per char
    uart.tick(2100);
    CHECK_EQ(sent.size(), 0u);
    uart.tick(40);
    CHECK_EQ(sent, std::string("A"));
    uart.tick(2140);
    CHECK_EQ(sent, std::string("AB"));
    CHECK_EQ(uart.read(5) & 0x60, 0x60);
}

TEST_CASE("8250 receive sets data ready and raises an interrupt") {
    Ins8250 uart(Machine::kCpuHz);
    uart.write(3, 0x80);
    uart.write(0, 12);
    uart.write(3, 0x03);
    uart.write(1, 0x01); // receive-data interrupt
    uart.receive('x');
    CHECK_EQ(uart.read(5) & 0x01, 0);
    uart.tick(2200);
    CHECK_EQ(uart.read(5) & 0x01, 1);
    CHECK_EQ(uart.irq(), true);
    CHECK_EQ(uart.read(2), 0x04);
    CHECK_EQ(uart.read(0), 'x');
    CHECK_EQ(uart.irq(), false);
}

TEST_CASE("2 ms clock interrupts at 500 Hz via RST 1") {
    RomBuilder b;
    b.at(0x0000, {0xC3, 0x40, 0x00});                   // JP 0040h
    b.at(0x0008, {0xE5, 0x2A, 0x00, 0x21, 0x23,         // RST 1: INC word (2100h)
                  0x22, 0x00, 0x21, 0xE1, 0xFB, 0xC9});
    b.at(0x0040, {0x31, 0x00, 0x30,                     // LD SP,3000h
                  0x3E, 0x02, 0xD3, 0xF2,               // enable clock via GPP
                  0xFB, 0x76, 0x18, 0xFD});             // EI; HALT; JR -3
    MachineConfig cfg;
    cfg.monitorRom = b.rom;
    Machine m(cfg);
    m.run(Machine::kCpuHz); // one second
    uint16_t ticks = read16(m, 0x2100);
    CHECK_EQ(ticks >= 499 && ticks <= 501, true);
}

TEST_CASE("Clock interrupt stays off until enabled in the GPP") {
    RomBuilder b;
    b.at(0x0000, {0x31, 0x00, 0x30, 0xFB, 0x76, 0x18, 0xFD});
    b.at(0x0008, {0x3E, 0x01, 0x32, 0x00, 0x21, 0x76});  // marks 2100h if taken
    MachineConfig cfg;
    cfg.monitorRom = b.rom;
    Machine m(cfg);
    m.run(Machine::kCpuHz / 10);
    CHECK_EQ(m.peek(0x2100), 0);
}

TEST_CASE("ORG-0 maps RAM over the ROM area") {
    RomBuilder b;
    // Copy the stub at 0100h to 3000h and run it from RAM, since the ROM
    // disappears from under the CPU once ORG-0 is selected.
    b.at(0x0000, {0x21, 0x00, 0x01, 0x11, 0x00, 0x30, 0x01, 0x0A, 0x00, 0xED, 0xB0,
                  0xC3, 0x00, 0x30});
    // Stub: LD A,20h; OUT (F2h),A; LD A,5Ah; LD (0010h),A; HALT
    b.at(0x0100, {0x3E, 0x20, 0xD3, 0xF2, 0x3E, 0x5A, 0x32, 0x10, 0x00, 0x76});
    b.at(0x0010, {0xAA});
    MachineConfig cfg;
    cfg.monitorRom = b.rom;
    Machine m(cfg);
    CHECK_EQ(m.peek(0x0010), 0xAA); // ROM content
    m.run(1000);
    CHECK_EQ(m.cpu().halted(), true);
    CHECK_EQ(m.gpp(), 0x20);
    CHECK_EQ(m.peek(0x0010), 0x5A); // RAM content
}

TEST_CASE("RAM size limits the top of memory") {
    MachineConfig cfg;
    cfg.ramKB = 48;
    Machine m(cfg);
    // 48K occupies 2000h-DFFFh
    CHECK_EQ(m.peek(0xDFFF), 0x00);
    CHECK_EQ(m.peek(0xE000), 0xFF);
}

TEST_CASE("Console output reaches the host") {
    RomBuilder b;
    // Program the 8250 for 9600 8N1 and send 'H'
    b.at(0x0000, {0x3E, 0x80, 0xD3, 0xEB, 0x3E, 0x0C, 0xD3, 0xE8, 0x3E, 0x00, 0xD3, 0xE9,
                  0x3E, 0x03, 0xD3, 0xEB, 0x3E, 0x48, 0xD3, 0xE8, 0x76});
    MachineConfig cfg;
    cfg.monitorRom = b.rom;
    Machine m(cfg);
    std::string out;
    m.consoleOutput = [&](uint8_t c) { out += static_cast<char>(c); };
    m.run(5000);
    CHECK_EQ(out, std::string("H"));
}

TEST_CASE("Serial card echoes MODEM input to the printer port") {
    RomBuilder b;
    // 9600 8N1 on D8h (MODEM) and E0h (LP), then copy each byte received on
    // D8h to E0h.
    b.at(0x0000, {0x3E, 0x80, 0xD3, 0xDB, 0x3E, 0x0C, 0xD3, 0xD8, 0x3E, 0x00, 0xD3, 0xD9, 0x3E, 0x03, 0xD3, 0xDB,
                  0x3E, 0x80, 0xD3, 0xE3, 0x3E, 0x0C, 0xD3, 0xE0, 0x3E, 0x00, 0xD3, 0xE1, 0x3E, 0x03, 0xD3, 0xE3,
                  0xDB, 0xDD, 0xE6, 0x01, 0x28, 0xFA, 0xDB, 0xD8, 0xD3, 0xE0, 0x18, 0xF4});
    MachineConfig cfg;
    cfg.monitorRom = b.rom;
    Machine m(cfg);
    std::string in = "HELLO", out;
    size_t next = 0;
    m.serialSource = [&](int port) { return port == Machine::kModem && next < in.size() ? in[next++] : -1; };
    m.serialOutput = [&](int port, uint8_t c) {
        if (port == Machine::kPrinter) out += static_cast<char>(c);
    };
    m.run(Machine::kCpuHz / 10); // 100 ms: about 96 characters' time at 9600 baud
    CHECK_EQ(out, in);
}

int main() { return check::runAll(); }
