#pragma once

#include "decoder.hpp"

#include <string>

namespace arm11 {

// ARM data-processing bodies: ADD/SUB/MOV with optional flags, and flag-only CMP.
// Conditions belong to the emitter. Immediate/shifted operands, shifter carry,
// and architectural PC semantics remain outside this direct-Rm subset.
class ALUTranslator {
public:
    [[nodiscard]] std::string TranslateInstruction(const Instruction& inst) const;
};

} // namespace arm11
