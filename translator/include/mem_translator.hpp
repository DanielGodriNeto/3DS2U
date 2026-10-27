#pragma once

#include "decoder.hpp"

#include <string>

namespace arm11 {

// Emits word LDR/STR with positive, immediate, pre-indexed offsets and no
// writeback. Generated code expects CPUContext cpu and Memory memory instances.
// Conditional execution and architectural PC semantics remain the caller's job.
class MemoryTranslator {
public:
    [[nodiscard]] std::string TranslateInstruction(const Instruction& inst) const;
};

} // namespace arm11
