#pragma once

#include "decoder.hpp"

#include <string>

namespace arm11 {

// Emits immediate word LDR/STR with signed-direction offsets, pre/post indexing,
// and writeback. The block's memory parameter refers to engine-owned WiiUMemory.
// Conditions, PC semantics, and architectural register-overlap constraints remain
// the caller's job; privilege differences for LDRT/STRT are not modeled here.
class MemoryTranslator {
public:
    [[nodiscard]] std::string TranslateInstruction(const Instruction& inst) const;
};

} // namespace arm11
