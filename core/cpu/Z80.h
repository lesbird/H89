#pragma once

#include "StateFile.h"

#include <bit>
#include <cstdint>

static_assert(std::endian::native == std::endian::little,
              "Z80 register pairs assume a little-endian host");

namespace h89 {

// Everything the CPU can see. The machine implements this to wire up memory,
// I/O ports, and the interrupt acknowledge cycle.
class Z80Bus {
public:
    virtual ~Z80Bus() = default;
    virtual uint8_t memRead(uint16_t addr) = 0;
    virtual void memWrite(uint16_t addr, uint8_t value) = 0;
    virtual uint8_t ioRead(uint16_t port) = 0;
    virtual void ioWrite(uint16_t port, uint8_t value) = 0;
    // Byte placed on the data bus during an interrupt acknowledge. In IM 0 it
    // is executed as an opcode (the H89 supplies RST instructions); in IM 2 it
    // is the low byte of the vector address.
    virtual uint8_t intAck() { return 0xFF; }
};

union RegPair {
    uint16_t w;
    struct {
        uint8_t l, h;
    };
};

struct Z80Regs {
    RegPair af, bc, de, hl;
    RegPair af2, bc2, de2, hl2;
    RegPair ix, iy;
    uint16_t sp = 0, pc = 0;
    uint16_t wz = 0; // internal MEMPTR, leaks into undocumented flags
    uint8_t i = 0, r = 0;
    bool iff1 = false, iff2 = false;
    uint8_t im = 0;
};

class Z80 {
public:
    enum Flag : uint8_t {
        CF = 0x01, NF = 0x02, PF = 0x04, XF = 0x08,
        HF = 0x10, YF = 0x20, ZF = 0x40, SF = 0x80,
    };

    explicit Z80(Z80Bus& bus);

    void reset();
    // Executes one instruction (or accepts a pending interrupt) and returns
    // the number of T-states it took.
    int step();

    // Level-triggered maskable interrupt line.
    void setIrq(bool asserted) { irqLine_ = asserted; }
    // Edge-triggered non-maskable interrupt.
    void triggerNmi() { nmiPending_ = true; }

    bool halted() const { return halted_; }
    uint64_t cycles() const { return cycles_; }

    Z80Regs& regs() { return r_; }
    const Z80Regs& regs() const { return r_; }

    void saveState(StateWriter& w) const;
    void loadState(StateReader& r);

private:
    // Fetch/memory helpers
    uint8_t fetch() { return bus_.memRead(r_.pc++); }
    uint16_t fetch16();
    uint16_t read16(uint16_t addr);
    void write16(uint16_t addr, uint16_t v);
    void push(uint16_t v);
    uint16_t pop();
    void incR() { r_.r = (r_.r & 0x80) | ((r_.r + 1) & 0x7F); }

    // Register access honoring the active DD/FD prefix
    uint16_t& hlx();
    uint8_t getReg(int r);           // r != 6
    void setReg(int r, uint8_t v);   // r != 6
    uint8_t getRegNoIdx(int r);      // H/L even under a prefix
    void setRegNoIdx(int r, uint8_t v);
    uint16_t memOperand();           // (HL) or (IX+d), fetching d
    uint16_t& rp(int p);             // BC, DE, HL/IX/IY, SP
    uint16_t& rp2(int p);            // BC, DE, HL/IX/IY, AF
    bool cond(int y) const;

    // ALU
    void alu(int op, uint8_t v);
    uint8_t inc8(uint8_t v);
    uint8_t dec8(uint8_t v);
    uint16_t add16(uint16_t a, uint16_t b);
    void adc16(uint16_t v);
    void sbc16(uint16_t v);
    uint8_t rotShift(int op, uint8_t v);
    void daa();

    int execMain(uint8_t op);
    int execCB();
    int execIndexCB();
    int execED();
    int acceptInterrupt();

    uint8_t& A() { return r_.af.h; }
    uint8_t& F() { return r_.af.l; }

    Z80Bus& bus_;
    Z80Regs r_;
    int idx_ = 0; // 0 = HL, 1 = IX, 2 = IY
    bool halted_ = false;
    bool eiDelay_ = false;
    bool irqLine_ = false;
    bool nmiPending_ = false;
    uint64_t cycles_ = 0;
};

} // namespace h89
