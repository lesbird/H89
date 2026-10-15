#include "Crtc6845.h"

namespace h89 {

void Crtc6845::reset() {
    for (auto& r : r_) r = 0;
    addr_ = 0;
    // Sane defaults so a frame can be timed before the firmware programs us.
    r_[0] = 0x60; r_[1] = 0x50; r_[4] = 0x19; r_[5] = 0x04; r_[6] = 0x18; r_[9] = 0x09;
}

void Crtc6845::writeData(uint8_t v) {
    if (addr_ < 16) r_[addr_] = v;
}

uint8_t Crtc6845::readData() const {
    // Only the cursor and light-pen registers are readable.
    return (addr_ >= 14 && addr_ <= 17) ? r_[addr_] : 0x00;
}

void Crtc6845::saveState(StateWriter& w) const {
    w.begin("6845");
    w.bytes(r_, sizeof r_);
    w.u8(addr_);
    w.end();
}

void Crtc6845::loadState(StateReader& r) {
    if (!r.begin("6845")) return;
    std::vector<uint8_t> regs = r.bytes();
    if (regs.size() == sizeof r_) std::copy(regs.begin(), regs.end(), r_);
    else r.fail("bad 6845 registers");
    addr_ = r.u8();
    r.end();
}

} // namespace h89
