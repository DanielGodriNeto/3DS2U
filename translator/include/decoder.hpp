#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace arm11 {

// Raw ARM-state instruction; operand and control-flow decoding come later.
struct Instruction {
    std::uint32_t opcode{};
    std::uint32_t address{}; // Instruction address, not the ARM PC operand (+8).
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
