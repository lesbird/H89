#include "Z80Disasm.h"

#include <cstdio>

namespace h89 {

namespace {

const char* const kR[] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
const char* const kRP[] = {"BC", "DE", "HL", "SP"};
const char* const kRP2[] = {"BC", "DE", "HL", "AF"};
const char* const kCC[] = {"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};
const char* const kALU[] = {"ADD A,", "ADC A,", "SUB ", "SBC A,", "AND ", "XOR ", "OR ", "CP "};
const char* const kROT[] = {"RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"};
const char* const kIM[] = {"0", "0", "1", "2", "0", "0", "1", "2"};
const char* const kBLI[4][4] = {
    {"LDI", "CPI", "INI", "OUTI"},
    {"LDD", "CPD", "IND", "OUTD"},
    {"LDIR", "CPIR", "INIR", "OTIR"},
    {"LDDR", "CPDR", "INDR", "OTDR"},
};

std::string hex8(uint8_t v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02Xh", v);
    return buf;
}

std::string hex16(uint16_t v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04Xh", v);
    return buf;
}

struct Decoder {
    const std::function<uint8_t(uint16_t)>& read;
    uint16_t start;
    uint16_t pc;
    const char* index = nullptr; // "IX" / "IY" under a DD/FD prefix
    bool haveDisp = false;
    int8_t disp = 0;

    uint8_t next() { return read(pc++); }
    uint16_t next16() {
        uint16_t lo = next();
        return lo | (next() << 8);
    }

    std::string hl() { return index ? index : "HL"; }

    std::string memHL() {
        if (!index) return "(HL)";
        if (!haveDisp) {
            disp = static_cast<int8_t>(next());
            haveDisp = true;
        }
        char buf[16];
        std::snprintf(buf, sizeof buf, "(%s%c%02Xh)", index, disp < 0 ? '-' : '+',
                      disp < 0 ? -disp : disp);
        return buf;
    }

    // Register r; under a prefix H/L become IXH/IXL unless `plain`.
    std::string reg(int r, bool plain = false) {
        if (r == 6) return memHL();
        if (index && !plain && (r == 4 || r == 5)) return std::string(index) + (r == 4 ? "H" : "L");
        return kR[r];
    }

    std::string rp(int p) { return p == 2 ? hl() : kRP[p]; }
    std::string rp2(int p) { return p == 2 ? hl() : kRP2[p]; }

    std::string rel() {
        auto d = static_cast<int8_t>(next());
        return hex16(static_cast<uint16_t>(pc + d));
    }

    std::string main(uint8_t op) {
        const int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
        switch (x) {
        case 0:
            switch (z) {
            case 0:
                switch (y) {
                case 0: return "NOP";
                case 1: return "EX AF,AF'";
                case 2: return "DJNZ " + rel();
                case 3: return "JR " + rel();
                default: return std::string("JR ") + kCC[y - 4] + "," + rel();
                }
            case 1:
                if (q == 0) return "LD " + rp(p) + "," + hex16(next16());
                return "ADD " + hl() + "," + rp(p);
            case 2:
                switch (y) {
                case 0: return "LD (BC),A";
                case 1: return "LD A,(BC)";
                case 2: return "LD (DE),A";
                case 3: return "LD A,(DE)";
                case 4: return "LD (" + hex16(next16()) + ")," + hl();
                case 5: return "LD " + hl() + ",(" + hex16(next16()) + ")";
                case 6: return "LD (" + hex16(next16()) + "),A";
                default: return "LD A,(" + hex16(next16()) + ")";
                }
            case 3: return std::string(q ? "DEC " : "INC ") + rp(p);
            case 4: return "INC " + reg(y);
            case 5: return "DEC " + reg(y);
            case 6: {
                std::string dst = reg(y);
                return "LD " + dst + "," + hex8(next());
            }
            default: {
                static const char* const ops[] = {"RLCA", "RRCA", "RLA", "RRA", "DAA", "CPL", "SCF", "CCF"};
                return ops[y];
            }
            }
        case 1:
            if (op == 0x76) return "HALT";
            if (y == 6 || z == 6) return "LD " + reg(y, z == 6) + "," + reg(z, y == 6);
            return "LD " + reg(y) + "," + reg(z);
        case 2: return std::string(kALU[y]) + reg(z);
        default:
            switch (z) {
            case 0: return std::string("RET ") + kCC[y];
            case 1:
                if (q == 0) return "POP " + rp2(p);
                switch (p) {
                case 0: return "RET";
                case 1: return "EXX";
                case 2: return "JP (" + hl() + ")";
                default: return "LD SP," + hl();
                }
            case 2: return std::string("JP ") + kCC[y] + "," + hex16(next16());
            case 3:
                switch (y) {
                case 0: return "JP " + hex16(next16());
                case 1: return cb();
                case 2: return "OUT (" + hex8(next()) + "),A";
                case 3: return "IN A,(" + hex8(next()) + ")";
                case 4: return "EX (SP)," + hl();
                case 5: return "EX DE,HL";
                case 6: return "DI";
                default: return "EI";
                }
            case 4: return std::string("CALL ") + kCC[y] + "," + hex16(next16());
            case 5:
                if (q == 0) return "PUSH " + rp2(p);
                switch (p) {
                case 0: return "CALL " + hex16(next16());
                case 2: index = nullptr; return ed(next());
                default: index = p == 1 ? "IX" : "IY"; return main(next());
                }
            case 6: return std::string(kALU[y]) + hex8(next());
            default: return "RST " + hex8(y * 8);
            }
        }
    }

    std::string cb() {
        std::string mem;
        if (index) mem = memHL(); // DD CB d op: displacement precedes opcode
        uint8_t op = next();
        const int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
        std::string operand = index ? mem : kR[z];
        std::string text;
        switch (x) {
        case 0: text = std::string(kROT[y]) + " " + operand; break;
        case 1: return "BIT " + std::to_string(y) + "," + operand;
        case 2: text = "RES " + std::to_string(y) + "," + operand; break;
        default: text = "SET " + std::to_string(y) + "," + operand; break;
        }
        if (index && z != 6) text += std::string(",") + kR[z]; // undocumented copy
        return text;
    }

    std::string ed(uint8_t op) {
        const int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
        if (x == 1) {
            switch (z) {
            case 0: return y == 6 ? "IN (C)" : std::string("IN ") + kR[y] + ",(C)";
            case 1: return y == 6 ? "OUT (C),0" : std::string("OUT (C),") + kR[y];
            case 2: return std::string(q ? "ADC HL," : "SBC HL,") + kRP[p];
            case 3:
                if (q == 0) return "LD (" + hex16(next16()) + ")," + kRP[p];
                return std::string("LD ") + kRP[p] + ",(" + hex16(next16()) + ")";
            case 4: return "NEG";
            case 5: return y == 1 ? "RETI" : "RETN";
            case 6: return std::string("IM ") + kIM[y];
            default: {
                static const char* const ops[] = {"LD I,A", "LD R,A", "LD A,I", "LD A,R", "RRD", "RLD", "NOP", "NOP"};
                return ops[y];
            }
            }
        }
        if (x == 2 && z <= 3 && y >= 4) return kBLI[y - 4][z];
        return "DB EDh," + hex8(op);
    }
};

} // namespace

Disassembly disassembleZ80(const std::function<uint8_t(uint16_t)>& read, uint16_t pc) {
    Decoder d{read, pc, pc};
    Disassembly out;
    out.text = d.main(d.next());
    out.length = static_cast<uint16_t>(d.pc - pc);
    return out;
}

} // namespace h89
