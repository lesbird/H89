#pragma once

#include <cstdint>

namespace h89 {

// Motorola MC6845 CRT controller: register file plus the derived geometry.
// Raster timing is driven by the owner (see H19), which asks for frame
// dimensions from here.
class Crtc6845 {
public:
    void reset();

    void writeAddress(uint8_t v) { addr_ = v & 0x1F; }
    void writeData(uint8_t v);
    uint8_t readData() const;

    int horizontalTotal() const { return r_[0] + 1; }    // characters per line
    int horizontalDisplayed() const { return r_[1]; }
    int verticalTotal() const { return (r_[4] & 0x7F) + 1; } // character rows
    int verticalAdjust() const { return r_[5] & 0x1F; }
    int verticalDisplayed() const { return r_[6] & 0x7F; }
    int scanLinesPerRow() const { return (r_[9] & 0x1F) + 1; }
    int totalScanLines() const { return verticalTotal() * scanLinesPerRow() + verticalAdjust(); }

    // Cursor blink mode from R10 bits 6:5.
    enum class CursorMode { Steady, Hidden, Blink16, Blink32 };
    CursorMode cursorMode() const { return static_cast<CursorMode>((r_[10] >> 5) & 3); }
    int cursorStart() const { return r_[10] & 0x1F; }
    int cursorEnd() const { return r_[11] & 0x1F; }

    uint16_t startAddress() const { return ((r_[12] & 0x3F) << 8) | r_[13]; }
    uint16_t cursorAddress() const { return ((r_[14] & 0x3F) << 8) | r_[15]; }

    uint8_t reg(int i) const { return r_[i & 0x1F]; }

private:
    uint8_t r_[18]{};
    uint8_t addr_ = 0;
};

} // namespace h89
