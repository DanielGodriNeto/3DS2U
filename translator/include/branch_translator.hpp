#pragma once

#include "decoder.hpp"

#include <cstdint>
#include <string>

namespace arm11 {

// Initial ARM-state B/BX translation; conditional branches and BL are deferred.
class BranchTranslator {
public:
    // current_pc is the instruction address, before the ARM pipeline's +8.
    // BX emits the raw register target; Thumb interworking is not implemented.
    [[nodiscard]] std::string TranslateInstruction(
        const Instruction& inst, std::uint32_t current_pc) const;
};

} // namespace arm11
