#pragma once

#include "decoder.hpp"

#include <string>

namespace arm11 {

// Emits immediate word LDR/STR with signed-direction offsets, pre/post indexing,
// and writeback, plus ordinary LDM/STM. The memory parameter is engine-owned.
// Conditions, PC semantics, and architectural register-overlap constraints remain
// the caller's job; privilege differences for LDRT/STRT are not modeled here.
class MemoryTranslator {
public:
    [[nodiscard]] std::string TranslateInstruction(const Instruction& inst) const;

    // Supported LDM loading PC transfers control back to the dispatcher.
    [[nodiscard]] static constexpr bool IsLoadMultipleToPC(const Instruction& inst) noexcept {
        const auto raw = inst.opcode;
        return ((raw >> 25) & 7u) == 4u && (raw & (1u << 22)) == 0 &&
               (raw & (1u << 20)) != 0 && (raw & (1u << 15)) != 0 &&
               (raw >> 28) != 0xFu;
    }
};

} // namespace arm11
