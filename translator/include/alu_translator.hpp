#pragma once

#include "decoder.hpp"

#include <string>

namespace arm11 {

// ARM data-processing bodies: ADD/SUB/MOV/ORR with optional flags and flag-only CMP.
// Operand2 supports direct registers and rotated immediates. Conditions belong to
// the emitter; shifted registers and architectural PC semantics remain deferred.
class ALUTranslator {
public:
    [[nodiscard]] std::string TranslateInstruction(const Instruction& inst) const;
};

} // namespace arm11
