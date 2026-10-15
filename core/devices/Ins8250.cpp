#include "Ins8250.h"

namespace h89 {

void Ins8250::reset() {
    rbr_ = thr_ = tsr_ = 0;
    ier_ = lcr_ = mcr_ = scr_ = 0;
    lsr_ = LSR_THRE | LSR_TEMT;
    msr_ &= 0xF0; // keep the modem inputs, clear the delta bits
    dll_ = dlm_ = 0;
    thrFull_ = tsrBusy_ = false;
    threIntPending_ = false;
    txCountdown_ = rxCountdown_ = 0;
    rxQueue_.clear();
}

int Ins8250::charCycles() const {
    int dataBits = 5 + (lcr_ & 0x03);
    int stopBits = (lcr_ & 0x04) ? 2 : 1;
    int parity = (lcr_ & 0x08) ? 1 : 0;
    int bits = 1 + dataBits + parity + stopBits;
    uint16_t div = divisor() ? divisor() : 1;
    return static_cast<int>(int64_t(bits) * 16 * div * cpuHz_ / xtalHz_);
}

uint8_t Ins8250::interruptId() const {
    if ((ier_ & IER_RLS) && (lsr_ & LSR_OE)) return 0x06;
    if ((ier_ & IER_RDA) && (lsr_ & LSR_DR)) return 0x04;
    if ((ier_ & IER_THRE) && threIntPending_) return 0x02;
    if ((ier_ & IER_MS) && (msr_ & 0x0F)) return 0x00;
    return kIirNone;
}

uint8_t Ins8250::read(int reg) {
    switch (reg & 7) {
    case 0:
        if (lcr_ & LCR_DLAB) return dll_;
        lsr_ &= ~LSR_DR;
        return rbr_;
    case 1: return (lcr_ & LCR_DLAB) ? dlm_ : ier_;
    case 2: {
        uint8_t id = interruptId();
        if (id == 0x02) threIntPending_ = false;
        return id;
    }
    case 3: return lcr_;
    case 4: return mcr_;
    case 5: {
        uint8_t v = lsr_;
        lsr_ &= ~LSR_OE; // error bits clear on read
        return v;
    }
    case 6: {
        uint8_t v = msr_;
        msr_ &= 0xF0;
        return v;
    }
    default: return scr_;
    }
}

void Ins8250::write(int reg, uint8_t value) {
    switch (reg & 7) {
    case 0:
        if (lcr_ & LCR_DLAB) {
            dll_ = value;
            break;
        }
        thr_ = value;
        thrFull_ = true;
        threIntPending_ = false;
        lsr_ &= ~(LSR_THRE | LSR_TEMT);
        if (!tsrBusy_) { // move straight into the shift register
            tsr_ = thr_;
            thrFull_ = false;
            tsrBusy_ = true;
            txCountdown_ = charCycles();
            lsr_ |= LSR_THRE;
            threIntPending_ = true;
        }
        break;
    case 1:
        if (lcr_ & LCR_DLAB) {
            dlm_ = value;
        } else {
            // Enabling THRE interrupts while the holding register is empty
            // raises one immediately.
            if ((value & IER_THRE) && !(ier_ & IER_THRE) && (lsr_ & LSR_THRE))
                threIntPending_ = true;
            ier_ = value & 0x0F;
        }
        break;
    case 3: lcr_ = value; break;
    case 4: mcr_ = value & 0x1F; break;
    case 7: scr_ = value; break;
    default: break; // IIR, LSR, MSR are read-only
    }
}

void Ins8250::setModemInputs(uint8_t msrHighNibble) {
    uint8_t now = msrHighNibble & 0xF0;
    uint8_t changed = (msr_ ^ now) & 0xF0;
    uint8_t delta = 0;
    if (changed & 0x10) delta |= 0x01; // DCTS
    if (changed & 0x20) delta |= 0x02; // DDSR
    if ((msr_ & 0x40) && !(now & 0x40)) delta |= 0x04; // TERI: trailing edge
    if (changed & 0x80) delta |= 0x08; // DDCD
    msr_ = now | (msr_ & 0x0F) | delta;
}

void Ins8250::tick(int cycles) {
    if (tsrBusy_) {
        txCountdown_ -= cycles;
        if (txCountdown_ <= 0) {
            if (onTransmit) onTransmit(tsr_);
            if (thrFull_) {
                tsr_ = thr_;
                thrFull_ = false;
                txCountdown_ += charCycles();
                lsr_ |= LSR_THRE;
                threIntPending_ = true;
            } else {
                tsrBusy_ = false;
                lsr_ |= LSR_TEMT;
            }
        }
    }

    if (!rxQueue_.empty()) {
        if (rxCountdown_ <= 0) rxCountdown_ = charCycles();
        rxCountdown_ -= cycles;
        if (rxCountdown_ <= 0) {
            if (lsr_ & LSR_DR) lsr_ |= LSR_OE; // previous byte never read
            rbr_ = rxQueue_.front();
            rxQueue_.pop_front();
            lsr_ |= LSR_DR;
            rxCountdown_ = 0;
        }
    }
}

void Ins8250::saveState(StateWriter& w) const {
    w.begin("8250");
    for (uint8_t v : {rbr_, thr_, tsr_, ier_, lcr_, mcr_, lsr_, msr_, scr_, dll_, dlm_}) w.u8(v);
    w.boolean(thrFull_), w.boolean(tsrBusy_), w.boolean(threIntPending_);
    w.i32(txCountdown_), w.i32(rxCountdown_);
    w.bytes(rxQueue_);
    w.end();
}

void Ins8250::loadState(StateReader& r) {
    if (!r.begin("8250")) return;
    for (uint8_t* v : {&rbr_, &thr_, &tsr_, &ier_, &lcr_, &mcr_, &lsr_, &msr_, &scr_, &dll_, &dlm_}) *v = r.u8();
    thrFull_ = r.boolean(), tsrBusy_ = r.boolean(), threIntPending_ = r.boolean();
    txCountdown_ = r.i32(), rxCountdown_ = r.i32();
    r.bytes(rxQueue_);
    r.end();
}

} // namespace h89
