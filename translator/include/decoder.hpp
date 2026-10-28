#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace arm11 {

enum class InstructionSet : std::uint8_t { ARM, Thumb };

// Raw instruction plus execution state; Thumb opcodes occupy the low 16 bits.
struct Instruction {
    std::uint32_t opcode{};
    std::uint32_t address{}; // Canonical instruction address, before pipeline bias.
    InstructionSet state{InstructionSet::ARM};
    [[nodiscard]] constexpr std::uint32_t Width() const noexcept {
        return state == InstructionSet::Thumb ? 2u : 4u;
    }
};

class Decoder {
public:
    // Fetches 32-bit ARM instructions only; Thumb requires a separate fetch path.
    // Pass raw code or an extracted executable ELF region, never ELF headers.
    // Throws std::invalid_argument for misalignment or incomplete instructions,
    // and std::out_of_range if the region exceeds the 32-bit address space.
    [[nodiscard]] std::vector<Instruction>
    DecodeBuffer(std::span<const std::uint8_t> buffer,
                 std::uint32_t base_address = 0x08000000u) const;

    // Prints the guest instruction address and its raw opcode to std::cout.
    void PrintInstruction(const Instruction& inst) const;
};

} // namespace arm11
