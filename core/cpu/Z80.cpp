#include "Z80.h"

#include <utility>

namespace h89 {

namespace {

struct FlagTables {
    uint8_t sz53[256]{};  // sign, zero, undocumented bits 5 and 3
    uint8_t sz53p[256]{}; // ... plus even parity

    constexpr FlagTables() {
        for (int i = 0; i < 256; ++i) {
            uint8_t f = i & (Z80::SF | Z80::YF | Z80::XF);
            if (i == 0) f |= Z80::ZF;
            int parity = 0;
            for (int b = 0; b < 8; ++b) parity ^= (i >> b) & 1;
            sz53[i] = f;
            sz53p[i] = f | (parity ? 0 : Z80::PF);
        }
    }
};

constexpr FlagTables T;

} // namespace

Z80::Z80(Z80Bus& bus) : bus_(bus) { reset(); }

void Z80::reset() {
    r_ = Z80Regs{};
    r_.af.w = 0xFFFF;
    r_.sp = 0xFFFF;
    idx_ = 0;
    halted_ = false;
    eiDelay_ = false;
    nmiPending_ = false;
}

uint16_t Z80::fetch16() {
    uint16_t lo = fetch();
    return lo | (fetch() << 8);
}

uint16_t Z80::read16(uint16_t addr) {
    uint16_t lo = bus_.memRead(addr);
    return lo | (bus_.memRead(addr + 1) << 8);
}

void Z80::write16(uint16_t addr, uint16_t v) {
    bus_.memWrite(addr, v & 0xFF);
    bus_.memWrite(addr + 1, v >> 8);
}

void Z80::push(uint16_t v) {
    bus_.memWrite(--r_.sp, v >> 8);
    bus_.memWrite(--r_.sp, v & 0xFF);
}

uint16_t Z80::pop() {
    uint16_t v = read16(r_.sp);
    r_.sp += 2;
    return v;
}

uint16_t& Z80::hlx() {
    return idx_ == 0 ? r_.hl.w : (idx_ == 1 ? r_.ix.w : r_.iy.w);
}

uint8_t Z80::getReg(int r) {
    switch (r) {
    case 4: return hlx() >> 8;
    case 5: return hlx() & 0xFF;
    default: return getRegNoIdx(r);
    }
}

void Z80::setReg(int r, uint8_t v) {
    switch (r) {
    case 4: hlx() = (hlx() & 0x00FF) | (v << 8); break;
    case 5: hlx() = (hlx() & 0xFF00) | v; break;
    default: setRegNoIdx(r, v); break;
    }
}

uint8_t Z80::getRegNoIdx(int r) {
    switch (r) {
    case 0: return r_.bc.h;
    case 1: return r_.bc.l;
    case 2: return r_.de.h;
    case 3: return r_.de.l;
    case 4: return r_.hl.h;
    case 5: return r_.hl.l;
    default: return A();
    }
}

void Z80::setRegNoIdx(int r, uint8_t v) {
    switch (r) {
    case 0: r_.bc.h = v; break;
    case 1: r_.bc.l = v; break;
    case 2: r_.de.h = v; break;
    case 3: r_.de.l = v; break;
    case 4: r_.hl.h = v; break;
    case 5: r_.hl.l = v; break;
    default: A() = v; break;
    }
}

uint16_t Z80::memOperand() {
    if (idx_ == 0) return r_.hl.w;
    auto d = static_cast<int8_t>(fetch());
    r_.wz = hlx() + d;
    return r_.wz;
}

uint16_t& Z80::rp(int p) {
    switch (p) {
    case 0: return r_.bc.w;
    case 1: return r_.de.w;
    case 2: return hlx();
    default: return r_.sp;
    }
}

uint16_t& Z80::rp2(int p) {
    return p == 3 ? r_.af.w : rp(p);
}

bool Z80::cond(int y) const {
    uint8_t f = r_.af.l;
    switch (y) {
    case 0: return !(f & ZF);
    case 1: return f & ZF;
    case 2: return !(f & CF);
    case 3: return f & CF;
    case 4: return !(f & PF);
    case 5: return f & PF;
    case 6: return !(f & SF);
    default: return f & SF;
    }
}

void Z80::alu(int op, uint8_t v) {
    uint8_t a = A();
    switch (op) {
    case 0: // ADD
    case 1: { // ADC
        unsigned c = op == 1 ? (F() & CF) : 0;
        unsigned r = a + v + c;
        uint8_t res = r & 0xFF;
        F() = T.sz53[res] | ((a ^ v ^ r) & HF) |
              (((a ^ ~v) & (a ^ r) & 0x80) >> 5) | ((r >> 8) & CF);
        A() = res;
        break;
    }
    case 2: // SUB
    case 3: // SBC
    case 7: { // CP
        unsigned c = op == 3 ? (F() & CF) : 0;
        unsigned r = a - v - c;
        uint8_t res = r & 0xFF;
        uint8_t f = NF | ((a ^ v ^ r) & HF) | (((a ^ v) & (a ^ r) & 0x80) >> 5) |
                    ((r >> 8) & CF);
        if (op == 7) {
            // CP takes the undocumented bits from the operand, not the result.
            F() = f | (T.sz53[res] & (SF | ZF)) | (v & (XF | YF));
        } else {
            F() = f | T.sz53[res];
            A() = res;
        }
        break;
    }
    case 4:
        A() = a & v;
        F() = T.sz53p[A()] | HF;
        break;
    case 5:
        A() = a ^ v;
        F() = T.sz53p[A()];
        break;
    default:
        A() = a | v;
        F() = T.sz53p[A()];
        break;
    }
}

uint8_t Z80::inc8(uint8_t v) {
    uint8_t r = v + 1;
    F() = (F() & CF) | T.sz53[r] | (r == 0x80 ? PF : 0) | ((r & 0x0F) == 0 ? HF : 0);
    return r;
}

uint8_t Z80::dec8(uint8_t v) {
    uint8_t r = v - 1;
    F() = (F() & CF) | NF | T.sz53[r] | (r == 0x7F ? PF : 0) |
          ((v & 0x0F) == 0 ? HF : 0);
    return r;
}

uint16_t Z80::add16(uint16_t a, uint16_t b) {
    unsigned r = a + b;
    r_.wz = a + 1;
    F() = (F() & (SF | ZF | PF)) | ((r >> 8) & (XF | YF)) |
          (((a ^ b ^ r) >> 8) & HF) | ((r >> 16) & CF);
    return r & 0xFFFF;
}

void Z80::adc16(uint16_t v) {
    uint16_t hl = r_.hl.w;
    unsigned r = hl + v + (F() & CF);
    r_.wz = hl + 1;
    F() = ((r >> 8) & (SF | XF | YF)) | ((r & 0xFFFF) == 0 ? ZF : 0) |
          (((hl ^ v ^ r) >> 8) & HF) | (((hl ^ ~v) & (hl ^ r) & 0x8000) >> 13) |
          ((r >> 16) & CF);
    r_.hl.w = r & 0xFFFF;
}

void Z80::sbc16(uint16_t v) {
    uint16_t hl = r_.hl.w;
    unsigned r = hl - v - (F() & CF);
    r_.wz = hl + 1;
    F() = ((r >> 8) & (SF | XF | YF)) | ((r & 0xFFFF) == 0 ? ZF : 0) | NF |
          (((hl ^ v ^ r) >> 8) & HF) | (((hl ^ v) & (hl ^ r) & 0x8000) >> 13) |
          ((r >> 16) & CF);
    r_.hl.w = r & 0xFFFF;
}

uint8_t Z80::rotShift(int op, uint8_t v) {
    uint8_t c;
    switch (op) {
    case 0: c = v >> 7; v = (v << 1) | c; break;                 // RLC
    case 1: c = v & 1; v = (v >> 1) | (c << 7); break;           // RRC
    case 2: c = v >> 7; v = (v << 1) | (F() & CF); break;        // RL
    case 3: c = v & 1; v = (v >> 1) | ((F() & CF) << 7); break;  // RR
    case 4: c = v >> 7; v <<= 1; break;                          // SLA
    case 5: c = v & 1; v = (v >> 1) | (v & 0x80); break;         // SRA
    case 6: c = v >> 7; v = (v << 1) | 1; break;                 // SLL (undocumented)
    default: c = v & 1; v >>= 1; break;                          // SRL
    }
    F() = T.sz53p[v] | c;
    return v;
}

void Z80::daa() {
    uint8_t a = A();
    uint8_t f = F();
    uint8_t corr = 0;
    uint8_t carry = f & CF;
    if ((f & HF) || (a & 0x0F) > 9) corr |= 0x06;
    if (carry || a > 0x99) {
        corr |= 0x60;
        carry = CF;
    }
    bool half;
    if (f & NF) {
        half = (f & HF) && (a & 0x0F) < 6;
        a -= corr;
    } else {
        half = (a & 0x0F) > 9;
        a += corr;
    }
    A() = a;
    F() = T.sz53p[a] | (f & NF) | carry | (half ? HF : 0);
}

static void bitTest(uint8_t& f, int bit, uint8_t v, uint8_t xySource) {
    uint8_t r = (f & Z80::CF) | Z80::HF | (xySource & (Z80::XF | Z80::YF));
    if (!(v & (1 << bit)))
        r |= Z80::ZF | Z80::PF;
    else if (bit == 7)
        r |= Z80::SF;
    f = r;
}

int Z80::step() {
    int t;
    if (nmiPending_) {
        nmiPending_ = false;
        eiDelay_ = false;
        halted_ = false;
        r_.iff1 = false;
        incR();
        push(r_.pc);
        r_.pc = r_.wz = 0x0066;
        t = 11;
    } else if (irqLine_ && r_.iff1 && !eiDelay_) {
        t = acceptInterrupt();
    } else {
        eiDelay_ = false;
        incR();
        if (halted_) {
            t = 4;
        } else {
            idx_ = 0;
            t = execMain(fetch());
        }
    }
    cycles_ += t;
    return t;
}

int Z80::acceptInterrupt() {
    halted_ = false;
    r_.iff1 = r_.iff2 = false;
    incR();
    uint8_t data = bus_.intAck();
    switch (r_.im) {
    case 0:
        // Execute the byte on the bus. Only single-byte opcodes (RST) are
        // meaningful here; multi-byte ones would fetch operands from memory.
        idx_ = 0;
        return 2 + execMain(data);
    case 1:
        push(r_.pc);
        r_.pc = r_.wz = 0x0038;
        return 13;
    default:
        push(r_.pc);
        r_.pc = r_.wz = read16((r_.i << 8) | data);
        return 19;
    }
}

int Z80::execMain(uint8_t op) {
    int extra = 0;
    while (op == 0xDD || op == 0xFD) {
        idx_ = op == 0xDD ? 1 : 2;
        extra += 4;
        incR();
        op = fetch();
    }
    // (IX+d) memory operands cost 8 more than (HL)
    const int idxMem = idx_ ? 8 : 0;

    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;

    switch (x) {
    case 0:
        switch (z) {
        case 0:
            switch (y) {
            case 0: return 4 + extra;
            case 1: std::swap(r_.af.w, r_.af2.w); return 4 + extra;
            case 2: {
                auto d = static_cast<int8_t>(fetch());
                if (--r_.bc.h) {
                    r_.pc = r_.wz = r_.pc + d;
                    return 13 + extra;
                }
                return 8 + extra;
            }
            case 3: {
                auto d = static_cast<int8_t>(fetch());
                r_.pc = r_.wz = r_.pc + d;
                return 12 + extra;
            }
            default: {
                auto d = static_cast<int8_t>(fetch());
                if (cond(y - 4)) {
                    r_.pc = r_.wz = r_.pc + d;
                    return 12 + extra;
                }
                return 7 + extra;
            }
            }
        case 1:
            if (q == 0) {
                rp(p) = fetch16();
                return 10 + extra;
            }
            hlx() = add16(hlx(), rp(p));
            return 11 + extra;
        case 2:
            switch (y) {
            case 0:
            case 2: {
                uint16_t addr = y == 0 ? r_.bc.w : r_.de.w;
                bus_.memWrite(addr, A());
                r_.wz = ((addr + 1) & 0xFF) | (A() << 8);
                return 7 + extra;
            }
            case 1:
            case 3: {
                uint16_t addr = y == 1 ? r_.bc.w : r_.de.w;
                A() = bus_.memRead(addr);
                r_.wz = addr + 1;
                return 7 + extra;
            }
            case 4: {
                uint16_t nn = fetch16();
                write16(nn, hlx());
                r_.wz = nn + 1;
                return 16 + extra;
            }
            case 5: {
                uint16_t nn = fetch16();
                hlx() = read16(nn);
                r_.wz = nn + 1;
                return 16 + extra;
            }
            case 6: {
                uint16_t nn = fetch16();
                bus_.memWrite(nn, A());
                r_.wz = ((nn + 1) & 0xFF) | (A() << 8);
                return 13 + extra;
            }
            default: {
                uint16_t nn = fetch16();
                A() = bus_.memRead(nn);
                r_.wz = nn + 1;
                return 13 + extra;
            }
            }
        case 3:
            if (q == 0) ++rp(p);
            else --rp(p);
            return 6 + extra;
        case 4:
        case 5:
            if (y == 6) {
                uint16_t addr = memOperand();
                uint8_t v = bus_.memRead(addr);
                bus_.memWrite(addr, z == 4 ? inc8(v) : dec8(v));
                return 11 + extra + idxMem;
            }
            setReg(y, z == 4 ? inc8(getReg(y)) : dec8(getReg(y)));
            return 4 + extra;
        case 6:
            if (y == 6) {
                uint16_t addr = memOperand();
                bus_.memWrite(addr, fetch());
                return 10 + extra + (idx_ ? 5 : 0);
            }
            setReg(y, fetch());
            return 7 + extra;
        default: {
            uint8_t a = A();
            uint8_t keep = F() & (SF | ZF | PF);
            switch (y) {
            case 0: { // RLCA
                uint8_t c = a >> 7;
                a = (a << 1) | c;
                F() = keep | (a & (XF | YF)) | c;
                A() = a;
                break;
            }
            case 1: { // RRCA
                uint8_t c = a & 1;
                a = (a >> 1) | (c << 7);
                F() = keep | (a & (XF | YF)) | c;
                A() = a;
                break;
            }
            case 2: { // RLA
                uint8_t c = a >> 7;
                a = (a << 1) | (F() & CF);
                F() = keep | (a & (XF | YF)) | c;
                A() = a;
                break;
            }
            case 3: { // RRA
                uint8_t c = a & 1;
                a = (a >> 1) | ((F() & CF) << 7);
                F() = keep | (a & (XF | YF)) | c;
                A() = a;
                break;
            }
            case 4: daa(); break;
            case 5: // CPL
                A() = ~a;
                F() = (F() & (SF | ZF | PF | CF)) | HF | NF | (A() & (XF | YF));
                break;
            case 6: // SCF
                F() = keep | CF | (a & (XF | YF));
                break;
            default: // CCF
                F() = keep | ((F() & CF) ? HF : CF) | (a & (XF | YF));
                break;
            }
            return 4 + extra;
        }
        }

    case 1:
        if (op == 0x76) {
            halted_ = true;
            return 4 + extra;
        }
        if (y == 6) {
            uint16_t addr = memOperand();
            bus_.memWrite(addr, getRegNoIdx(z));
            return 7 + extra + idxMem;
        }
        if (z == 6) {
            uint16_t addr = memOperand();
            setRegNoIdx(y, bus_.memRead(addr));
            return 7 + extra + idxMem;
        }
        setReg(y, getReg(z));
        return 4 + extra;

    case 2:
        if (z == 6) {
            alu(y, bus_.memRead(memOperand()));
            return 7 + extra + idxMem;
        }
        alu(y, getReg(z));
        return 4 + extra;

    default:
        switch (z) {
        case 0:
            if (cond(y)) {
                r_.pc = r_.wz = pop();
                return 11 + extra;
            }
            return 5 + extra;
        case 1:
            if (q == 0) {
                rp2(p) = pop();
                return 10 + extra;
            }
            switch (p) {
            case 0:
                r_.pc = r_.wz = pop();
                return 10 + extra;
            case 1:
                std::swap(r_.bc.w, r_.bc2.w);
                std::swap(r_.de.w, r_.de2.w);
                std::swap(r_.hl.w, r_.hl2.w);
                return 4 + extra;
            case 2:
                r_.pc = hlx();
                return 4 + extra;
            default:
                r_.sp = hlx();
                return 6 + extra;
            }
        case 2: {
            uint16_t nn = fetch16();
            r_.wz = nn;
            if (cond(y)) r_.pc = nn;
            return 10 + extra;
        }
        case 3:
            switch (y) {
            case 0:
                r_.pc = r_.wz = fetch16();
                return 10 + extra;
            case 1:
                if (idx_) return extra + execIndexCB();
                incR();
                return extra + execCB();
            case 2: {
                uint8_t n = fetch();
                bus_.ioWrite((A() << 8) | n, A());
                r_.wz = ((n + 1) & 0xFF) | (A() << 8);
                return 11 + extra;
            }
            case 3: {
                uint16_t port = (A() << 8) | fetch();
                A() = bus_.ioRead(port);
                r_.wz = port + 1;
                return 11 + extra;
            }
            case 4: {
                uint16_t v = read16(r_.sp);
                write16(r_.sp, hlx());
                hlx() = r_.wz = v;
                return 19 + extra;
            }
            case 5:
                std::swap(r_.de.w, r_.hl.w); // never affected by DD/FD
                return 4 + extra;
            case 6:
                r_.iff1 = r_.iff2 = false;
                return 4 + extra;
            default:
                r_.iff1 = r_.iff2 = true;
                eiDelay_ = true;
                return 4 + extra;
            }
        case 4: {
            uint16_t nn = fetch16();
            r_.wz = nn;
            if (cond(y)) {
                push(r_.pc);
                r_.pc = nn;
                return 17 + extra;
            }
            return 10 + extra;
        }
        case 5:
            if (q == 0) {
                push(rp2(p));
                return 11 + extra;
            }
            if (p == 0) {
                uint16_t nn = fetch16();
                r_.wz = nn;
                push(r_.pc);
                r_.pc = nn;
                return 17 + extra;
            }
            // p == 2: ED prefix (DD/FD were consumed above). A preceding
            // DD/FD has no effect on ED instructions.
            idx_ = 0;
            incR();
            return extra + execED();
        case 6:
            alu(y, fetch());
            return 7 + extra;
        default:
            push(r_.pc);
            r_.pc = r_.wz = y * 8;
            return 11 + extra;
        }
    }
}

int Z80::execCB() {
    uint8_t op = fetch();
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7;

    if (z == 6) {
        uint16_t addr = r_.hl.w;
        uint8_t v = bus_.memRead(addr);
        switch (x) {
        case 0: bus_.memWrite(addr, rotShift(y, v)); return 15;
        case 1: bitTest(F(), y, v, r_.wz >> 8); return 12;
        case 2: bus_.memWrite(addr, v & ~(1 << y)); return 15;
        default: bus_.memWrite(addr, v | (1 << y)); return 15;
        }
    }

    uint8_t v = getRegNoIdx(z);
    switch (x) {
    case 0: setRegNoIdx(z, rotShift(y, v)); break;
    case 1: bitTest(F(), y, v, v); break;
    case 2: setRegNoIdx(z, v & ~(1 << y)); break;
    default: setRegNoIdx(z, v | (1 << y)); break;
    }
    return 8;
}

// DD CB d op / FD CB d op. Returns cycles excluding the 4 for the DD/FD prefix.
int Z80::execIndexCB() {
    auto d = static_cast<int8_t>(fetch());
    uint8_t op = fetch(); // not an M1 cycle, so R is not incremented
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7;

    uint16_t addr = hlx() + d;
    r_.wz = addr;
    uint8_t v = bus_.memRead(addr);

    switch (x) {
    case 0: v = rotShift(y, v); break;
    case 1: bitTest(F(), y, v, addr >> 8); return 16;
    case 2: v &= ~(1 << y); break;
    default: v |= (1 << y); break;
    }
    bus_.memWrite(addr, v);
    // Undocumented: the result is also copied into a register when z != 6.
    if (z != 6) setRegNoIdx(z, v);
    return 19;
}

int Z80::execED() {
    uint8_t op = fetch();
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;

    if (x == 1) {
        switch (z) {
        case 0: {
            uint8_t v = bus_.ioRead(r_.bc.w);
            r_.wz = r_.bc.w + 1;
            F() = (F() & CF) | T.sz53p[v];
            if (y != 6) setRegNoIdx(y, v); // ED 70 only sets flags
            return 12;
        }
        case 1:
            bus_.ioWrite(r_.bc.w, y == 6 ? 0 : getRegNoIdx(y));
            r_.wz = r_.bc.w + 1;
            return 12;
        case 2:
            if (q == 0) sbc16(rp(p));
            else adc16(rp(p));
            return 15;
        case 3: {
            uint16_t nn = fetch16();
            if (q == 0) write16(nn, rp(p));
            else rp(p) = read16(nn);
            r_.wz = nn + 1;
            return 20;
        }
        case 4: {
            uint8_t v = A();
            A() = 0;
            alu(2, v); // NEG
            return 8;
        }
        case 5: // RETN / RETI
            r_.iff1 = r_.iff2;
            r_.pc = r_.wz = pop();
            return 14;
        case 6: {
            static constexpr uint8_t modes[4] = {0, 0, 1, 2};
            r_.im = modes[y & 3];
            return 8;
        }
        default:
            switch (y) {
            case 0: r_.i = A(); return 9;
            case 1: r_.r = A(); return 9;
            case 2:
            case 3:
                A() = y == 2 ? r_.i : r_.r;
                F() = (F() & CF) | T.sz53[A()] | (r_.iff2 ? PF : 0);
                return 9;
            case 4: { // RRD
                uint8_t v = bus_.memRead(r_.hl.w);
                bus_.memWrite(r_.hl.w, (A() << 4) | (v >> 4));
                A() = (A() & 0xF0) | (v & 0x0F);
                F() = (F() & CF) | T.sz53p[A()];
                r_.wz = r_.hl.w + 1;
                return 18;
            }
            case 5: { // RLD
                uint8_t v = bus_.memRead(r_.hl.w);
                bus_.memWrite(r_.hl.w, (v << 4) | (A() & 0x0F));
                A() = (A() & 0xF0) | (v >> 4);
                F() = (F() & CF) | T.sz53p[A()];
                r_.wz = r_.hl.w + 1;
                return 18;
            }
            default: return 8;
            }
        }
    }

    if (x == 2 && z <= 3 && y >= 4) {
        const int dir = (y & 1) ? -1 : 1;
        const bool repeat = y >= 6;

        switch (z) {
        case 0: { // LDI / LDD / LDIR / LDDR
            uint8_t v = bus_.memRead(r_.hl.w);
            bus_.memWrite(r_.de.w, v);
            r_.hl.w += dir;
            r_.de.w += dir;
            --r_.bc.w;
            uint8_t n = v + A();
            F() = (F() & (SF | ZF | CF)) | (r_.bc.w ? PF : 0) | (n & XF) | ((n << 4) & YF);
            if (repeat && r_.bc.w) {
                r_.pc -= 2;
                r_.wz = r_.pc + 1;
                return 21;
            }
            return 16;
        }
        case 1: { // CPI / CPD / CPIR / CPDR
            uint8_t v = bus_.memRead(r_.hl.w);
            uint8_t r = A() - v;
            uint8_t hf = (A() ^ v ^ r) & HF;
            r_.hl.w += dir;
            --r_.bc.w;
            r_.wz += dir;
            uint8_t n = r - (hf ? 1 : 0);
            F() = (F() & CF) | NF | (T.sz53[r] & (SF | ZF)) | hf | (r_.bc.w ? PF : 0) |
                  (n & XF) | ((n << 4) & YF);
            if (repeat && r_.bc.w && r != 0) {
                r_.pc -= 2;
                r_.wz = r_.pc + 1;
                return 21;
            }
            return 16;
        }
        case 2: { // INI / IND / INIR / INDR
            uint8_t v = bus_.ioRead(r_.bc.w);
            r_.wz = r_.bc.w + dir;
            bus_.memWrite(r_.hl.w, v);
            --r_.bc.h;
            r_.hl.w += dir;
            unsigned t = v + ((r_.bc.l + dir) & 0xFF);
            F() = T.sz53[r_.bc.h] | ((v & 0x80) ? NF : 0) | (t > 0xFF ? (HF | CF) : 0) |
                  (T.sz53p[(t & 7) ^ r_.bc.h] & PF);
            if (repeat && r_.bc.h) {
                r_.pc -= 2;
                return 21;
            }
            return 16;
        }
        default: { // OUTI / OUTD / OTIR / OTDR
            uint8_t v = bus_.memRead(r_.hl.w);
            --r_.bc.h;
            r_.wz = r_.bc.w + dir;
            bus_.ioWrite(r_.bc.w, v);
            r_.hl.w += dir;
            unsigned t = v + r_.hl.l;
            F() = T.sz53[r_.bc.h] | ((v & 0x80) ? NF : 0) | (t > 0xFF ? (HF | CF) : 0) |
                  (T.sz53p[(t & 7) ^ r_.bc.h] & PF);
            if (repeat && r_.bc.h) {
                r_.pc -= 2;
                return 21;
            }
            return 16;
        }
        }
    }

    return 8; // undefined ED opcodes act as 2-byte NOPs
}

} // namespace h89
