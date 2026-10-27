#include "alu_translator.hpp"

#include <cstdint>

namespace arm11 {

std::string ALUTranslator::TranslateInstruction(const Instruction& inst) const {
    const std::uint32_t raw = inst.opcode;

    // Initial class filter only; ARM miscellaneous encodings share bits 27:26.
    if (((raw >> 26) & 0b11u) != 0) {
        return "// Unhandled Instruction";
    }

    const auto opcode = (raw >> 21) & 0xFu;
    [[maybe_unused]] const bool set_flags = ((raw >> 20) & 1u) != 0;
    const auto rn = (raw >> 16) & 0xFu;
    const auto rd = (raw >> 12) & 0xFu;
    const auto operand2 = raw & 0xFFFu;
    const auto rm = operand2 & 0xFu;

    // Operand2 is deliberately treated as an unshifted register at this stage.
    // S is extracted above; flag generation will use CPUContext helpers later.
    const auto reg = [](std::uint32_t index) {
        return "cpu.r[" + std::to_string(index) + "]";
    };

    switch (opcode) {
    case 0b0100: // ADD: uint32_t register arithmetic wraps at 32 bits.
        return reg(rd) + " = " + reg(rn) + " + " + reg(rm) + ";";
    case 0b0010: // SUB
        return reg(rd) + " = " + reg(rn) + " - " + reg(rm) + ";";
    case 0b1101: // MOV does not consume Rn.
        return reg(rd) + " = " + reg(rm) + ";";
    default:
        return "// Unhandled Instruction";
    }
}

} // namespace arm11
