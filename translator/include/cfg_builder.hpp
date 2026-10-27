#pragma once

#include "decoder.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace arm11 {

enum class InstructionSet : std::uint8_t { ARM, Thumb };

// Canonical guest address plus execution state for future ARM/Thumb interworking.
struct CodeLocation {
    std::uint32_t address{}; // Bit 0 cleared; ARM addresses are 4-byte aligned.
    InstructionSet state{InstructionSet::ARM};
};

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
    // Input must contain aligned ARM instructions in strictly increasing order.
    // B/BL/BX terminate blocks; gaps and in-buffer conditional/AL B targets start
    // blocks. Invalid input throws std::invalid_argument. CFG edges are deferred.
    [[nodiscard]] std::unordered_map<std::uint32_t, BasicBlock>
    BuildBlocks(const std::vector<Instruction>& instructions) const;

    // Address-sorted diagnostics keep AOT translation runs reproducible.
    void PrintBlocks(const std::unordered_map<std::uint32_t, BasicBlock>& blocks) const;
};

} // namespace arm11
