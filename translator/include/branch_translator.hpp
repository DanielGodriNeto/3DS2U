#pragma once

#include "decoder.hpp"

#include <cstdint>
#include <string>

namespace arm11 {

// ARM-state B/BL/BX/BLX bodies; the emitter supplies CPSR condition guards.
class BranchTranslator {
public:
    // current_pc is the instruction address, before the ARM pipeline's +8.
    // BX/BLX exchange execution state using target bit zero.
    [[nodiscard]] std::string TranslateInstruction(
        const Instruction& inst, std::uint32_t current_pc) const;
};

} // namespace arm11
