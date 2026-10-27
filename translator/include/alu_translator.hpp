#pragma once

#include "decoder.hpp"

#include <string>

namespace arm11 {

// Initial ARM data-processing emitter: ADD, SUB, and MOV with direct Rm.
// Condition execution, shifted/immediate operands, PC semantics, and CPSR flag
// updates are deferred; callers must limit execution to the supported subset.
class ALUTranslator {
public:
    [[nodiscard]] std::string TranslateInstruction(const Instruction& inst) const;
};

} // namespace arm11
