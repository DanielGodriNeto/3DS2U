#pragma once

#include "decoder.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace arm11 {

// Canonical guest address plus execution state for ARM/Thumb interworking.
struct CodeLocation {
    std::uint32_t address{}; // Bit 0 cleared; ARM addresses are 4-byte aligned.
    InstructionSet state{InstructionSet::ARM};
};

// The low bit tags Thumb dispatcher entries; architectural PCs remain canonical.
[[nodiscard]] constexpr std::uint32_t DispatchKey(CodeLocation location) noexcept {
    return location.address | (location.state == InstructionSet::Thumb ? 1u : 0u);
}

// Shared block definition, also exposed through arm11_types.hpp.
struct BasicBlock {
    std::uint32_t start_address{};
    std::vector<Instruction> instructions;

    // Retain the analysis metadata introduced with the disassembler interface.
    CodeLocation start{};
    std::uint32_t byte_size{};
    std::uint32_t instruction_count{};
    std::vector<CodeLocation> successors;
    bool has_indirect_successor{};
};

class CFGBuilder {
public:
    // Instructions must be ordered/non-overlapping within each execution state.
    // Map keys use DispatchKey: ARM addresses are even, Thumb addresses are tagged.
    // Strides follow Instruction::Width(); direct targets split same-mode blocks.
    // Dynamic exchanges/returns are marked; other CFG edges remain deferred.
    // Invalid input throws std::invalid_argument.
    [[nodiscard]] std::unordered_map<std::uint32_t, BasicBlock>
    BuildBlocks(const std::vector<Instruction>& instructions) const;

    // Address-sorted diagnostics keep AOT translation runs reproducible.
    void PrintBlocks(const std::unordered_map<std::uint32_t, BasicBlock>& blocks) const;
};

} // namespace arm11
