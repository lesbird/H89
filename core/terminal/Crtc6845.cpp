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

} // namespace h89
