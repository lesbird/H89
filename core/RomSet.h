#pragma once

#include "Machine.h"

#include <string>

namespace h89 {

struct RomScan {
    MachineConfig config;
    std::string error;   // non-empty if a required ROM is missing
    std::string summary; // which files were picked, for display
    bool ok() const { return error.empty(); }
};

// Builds a MachineConfig from the ROM images in `dir`, recognised by the
// Heathkit part number in their file names (raw binaries only, not .hex):
//   monitor  444-142 (MTR-90), else 444-62 (MTR-89)   required
//   H17      444-19                                   optional
//   H19      444-46 firmware and 444-29 font          required
RomScan scanRomDirectory(const std::string& dir);

} // namespace h89
