#pragma once

#include "StateFile.h"

#include <cstdint>
#include <deque>
#include <functional>

namespace h89 {

// National INS8250 UART. Character timing is converted from the UART's own
// clock to CPU cycles so the transmitter-empty and data-ready bits behave at
// the programmed baud rate.
class Ins8250 {
public:
    static constexpr int kDefaultXtalHz = 1'843'200;

    explicit Ins8250(int cpuHz, int xtalHz = kDefaultXtalHz) : cpuHz_(cpuHz), xtalHz_(xtalHz) {
        reset();
    }

    void reset();

    uint8_t read(int reg);
    void write(int reg, uint8_t value);

    // Advances the shift registers by `cycles` CPU cycles.
    void tick(int cycles);

    // True while the UART is requesting an interrupt.
    bool irq() const { return interruptId() != kIirNone; }

    // Called with each byte once it has finished shifting out.
    std::function<void(uint8_t)> onTransmit;

    // Queues a byte from the remote end; it arrives one character time later.
    void receive(uint8_t byte) { rxQueue_.push_back(byte); }
    bool receiveIdle() const { return rxQueue_.empty(); }
    // Idle and the last byte has been read: another can come without overrun.
    bool readyForInput() const { return rxQueue_.empty() && !(lsr_ & LSR_DR); }

    // Modem status inputs (CTS, DSR, RI, DCD in MSR bits 4-7).
    void setModemInputs(uint8_t msrHighNibble);

    uint16_t divisor() const { return dll_ | (dlm_ << 8); }
    uint8_t lineControl() const { return lcr_; }  // bit 6: sending a break
    uint8_t modemControl() const { return mcr_; } // bit 0 DTR, bit 1 RTS

    // Characters take this many CPU cycles whatever the divisor says (0 =
    // follow the divisor). For a UART whose line is really a host serial port
    // running at its own rate.
    void setFixedCharCycles(int cycles) { fixedCharCycles_ = cycles; }

    void saveState(StateWriter& w) const;
    void loadState(StateReader& r);

private:
    static constexpr uint8_t kIirNone = 0x01;

    enum : uint8_t {
        LSR_DR = 0x01, LSR_OE = 0x02, LSR_THRE = 0x20, LSR_TEMT = 0x40,
        IER_RDA = 0x01, IER_THRE = 0x02, IER_RLS = 0x04, IER_MS = 0x08,
        LCR_DLAB = 0x80,
    };

    uint8_t interruptId() const;
    int charCycles() const; // CPU cycles to shift one character

    int cpuHz_;
    int xtalHz_;
    uint8_t rbr_ = 0, thr_ = 0, tsr_ = 0;
    uint8_t ier_ = 0, lcr_ = 0, mcr_ = 0, lsr_ = 0, msr_ = 0, scr_ = 0;
    uint8_t dll_ = 0, dlm_ = 0;
    bool thrFull_ = false, tsrBusy_ = false;
    bool threIntPending_ = false; // cleared by reading IIR or writing THR
    int txCountdown_ = 0;
    int rxCountdown_ = 0;
    int fixedCharCycles_ = 0; // host configuration, not machine state
    std::deque<uint8_t> rxQueue_;
};

} // namespace h89
