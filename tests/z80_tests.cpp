#include "check.h"
#include "cpu/Z80.h"

#include <array>
#include <initializer_list>

using h89::Z80;

namespace {

struct TestBus : h89::Z80Bus {
    std::array<uint8_t, 65536> mem{};
    uint8_t lastOutPort = 0, lastOutValue = 0;
    uint8_t ackByte = 0xCF; // RST 08h, as the H89 2 ms timer supplies

    uint8_t memRead(uint16_t a) override { return mem[a]; }
    void memWrite(uint16_t a, uint8_t v) override { mem[a] = v; }
    uint8_t ioRead(uint16_t port) override { return port & 0xFF; }
    void ioWrite(uint16_t port, uint8_t v) override {
        lastOutPort = port & 0xFF;
        lastOutValue = v;
    }
    uint8_t intAck() override { return ackByte; }

    void load(uint16_t at, std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) mem[at++] = b;
    }
};

struct Fixture {
    TestBus bus;
    Z80 cpu{bus};
    Fixture() { cpu.regs().sp = 0xF000; }
};

} // namespace

TEST_CASE("LD and ADD with flags and timing") {
    Fixture f;
    f.bus.load(0, {0x3E, 0x7F, 0xC6, 0x01}); // LD A,7Fh ; ADD A,1
    CHECK_EQ(f.cpu.step(), 7);
    CHECK_EQ(f.cpu.step(), 7);
    CHECK_EQ(f.cpu.regs().af.h, 0x80);
    CHECK_EQ(f.cpu.regs().af.l, Z80::SF | Z80::HF | Z80::PF); // overflow
}

TEST_CASE("DJNZ loop timing") {
    Fixture f;
    f.bus.load(0, {0x06, 0x03, 0x10, 0xFE}); // LD B,3 ; DJNZ $
    int t = 0;
    for (int i = 0; i < 4; ++i) t += f.cpu.step();
    CHECK_EQ(t, 7 + 13 + 13 + 8);
    CHECK_EQ(f.cpu.regs().pc, 4);
}

TEST_CASE("IX indexed load and store") {
    Fixture f;
    f.bus.load(0, {
        0xDD, 0x21, 0x00, 0x20,       // LD IX,2000h
        0xDD, 0x36, 0x05, 0x42,       // LD (IX+5),42h
        0xDD, 0x7E, 0x05,             // LD A,(IX+5)
        0xDD, 0x34, 0xFF,             // INC (IX-1)
    });
    CHECK_EQ(f.cpu.step(), 14);
    CHECK_EQ(f.cpu.step(), 19);
    CHECK_EQ(f.cpu.step(), 19);
    CHECK_EQ(f.cpu.step(), 23);
    CHECK_EQ(f.bus.mem[0x2005], 0x42);
    CHECK_EQ(f.cpu.regs().af.h, 0x42);
    CHECK_EQ(f.bus.mem[0x1FFF], 0x01);
}

TEST_CASE("CALL and RET") {
    Fixture f;
    f.bus.load(0, {0xCD, 0x00, 0x10});
    f.bus.load(0x1000, {0xC9});
    CHECK_EQ(f.cpu.step(), 17);
    CHECK_EQ(f.cpu.regs().pc, 0x1000);
    CHECK_EQ(f.cpu.step(), 10);
    CHECK_EQ(f.cpu.regs().pc, 3);
    CHECK_EQ(f.cpu.regs().sp, 0xF000);
}

TEST_CASE("LDIR block copy") {
    Fixture f;
    f.bus.load(0, {0x21, 0x00, 0x30, 0x11, 0x00, 0x40, 0x01, 0x03, 0x00, 0xED, 0xB0});
    f.bus.load(0x3000, {1, 2, 3});
    for (int i = 0; i < 3; ++i) f.cpu.step();
    int t = 0;
    while (f.cpu.regs().pc != 11) t += f.cpu.step();
    CHECK_EQ(t, 21 + 21 + 16);
    CHECK_EQ(f.bus.mem[0x4002], 3);
    CHECK_EQ(f.cpu.regs().bc.w, 0);
}

TEST_CASE("OUT and IN") {
    Fixture f;
    f.bus.load(0, {0x3E, 0x55, 0xD3, 0xE8, 0xDB, 0xF2});
    f.cpu.step();
    CHECK_EQ(f.cpu.step(), 11);
    CHECK_EQ(f.bus.lastOutPort, 0xE8);
    CHECK_EQ(f.bus.lastOutValue, 0x55);
    f.cpu.step();
    CHECK_EQ(f.cpu.regs().af.h, 0xF2); // test bus echoes the port number
}

TEST_CASE("IM 0 interrupt executes RST from the bus") {
    Fixture f;
    // EI ; HALT
    f.bus.load(0x100, {0xFB, 0x76});
    f.cpu.regs().pc = 0x100;
    f.cpu.setIrq(true);
    f.cpu.step(); // EI: interrupt is held off for one instruction
    CHECK_EQ(f.cpu.regs().pc, 0x101);
    f.cpu.step(); // HALT
    CHECK_EQ(f.cpu.halted(), true);
    CHECK_EQ(f.cpu.step(), 13); // RST 08h acknowledged
    CHECK_EQ(f.cpu.regs().pc, 0x08);
    CHECK_EQ(f.cpu.halted(), false);
    // Return address is the instruction after HALT
    CHECK_EQ(f.bus.mem[f.cpu.regs().sp], 0x02);
    CHECK_EQ(f.bus.mem[f.cpu.regs().sp + 1], 0x01);
    CHECK_EQ(f.cpu.regs().iff1, false);
}

TEST_CASE("IM 2 vectored interrupt") {
    Fixture f;
    f.bus.ackByte = 0x10;
    f.bus.load(0, {0x3E, 0x12, 0xED, 0x47, 0xED, 0x5E, 0xFB, 0x00});
    f.bus.load(0x1210, {0x34, 0x12});
    for (int i = 0; i < 5; ++i) f.cpu.step();
    f.cpu.setIrq(true);
    CHECK_EQ(f.cpu.step(), 19);
    CHECK_EQ(f.cpu.regs().pc, 0x1234);
}

TEST_CASE("DAA after BCD add") {
    Fixture f;
    f.bus.load(0, {0x3E, 0x19, 0xC6, 0x28, 0x27}); // 19 + 28 = 47 BCD
    for (int i = 0; i < 3; ++i) f.cpu.step();
    CHECK_EQ(f.cpu.regs().af.h, 0x47);
}

int main() { return check::runAll(); }
