#include "alu_translator.hpp"

#include <cstdint>
#include <bit>
#include <iomanip>
#include <sstream>

namespace arm11 {

std::string ALUTranslator::TranslateInstruction(const Instruction& inst) const {
    const std::uint32_t raw = inst.opcode;

    // Initial class filter only; ARM miscellaneous encodings share bits 27:26.
    if (((raw >> 26) & 0b11u) != 0) {
        return "// Unhandled Instruction";
    }

    const auto opcode = (raw >> 21) & 0xFu;
    const bool set_flags = ((raw >> 20) & 1u) != 0;
    const auto rn = (raw >> 16) & 0xFu;
    const auto rd = (raw >> 12) & 0xFu;
    const auto operand2 = raw & 0xFFFu;
    const auto rm = operand2 & 0xFu;
    const bool immediate = ((raw >> 25) & 1u) != 0;
    const auto rotation = static_cast<int>(((raw >> 8) & 0xFu) * 2u);
    const auto immediate_value = std::rotr(raw & 0xFFu, rotation);

    // Register Operand2 remains unshifted; immediate Operand2 uses ARM ROR encoding.
    const auto reg = [](std::uint32_t index) {
        return "cpu.r[" + std::to_string(index) + "]";
    };

    const bool add = opcode == 0b0100u;
    const bool sub = opcode == 0b0010u;
    const bool cmp = opcode == 0b1010u;
    const bool mov = opcode == 0b1101u;
    const bool orr = opcode == 0b1100u;
    if (!add && !sub && !cmp && !mov && !orr) {
        return "// Unhandled Instruction";
    }

    const auto lhs = reg(rn);
    std::ostringstream literal;
    literal << "0x" << std::hex << std::uppercase << immediate_value << 'u';
    const auto rhs = immediate ? literal.str() : reg(rm);
    const auto expression = mov ? rhs : lhs + (add ? " + " : orr ? " | " : " - ") + rhs;
    if (!set_flags && !cmp) {
        return reg(rd) + " = " + expression + ";";
    }

    // Per-instruction scope permits consecutive flag-setting operations to use
    // the same temporary, including inside the emitter's conditional guards.
    std::string code = "{\n    const std::uint32_t temp = " + expression + ";\n"
                       "    cpu.cpsr.N = ((temp >> 31) & 1u) != 0;\n"
                       "    cpu.cpsr.Z = (temp == 0);\n";
    if (add) {
        code += "    cpu.cpsr.C = (temp < " + lhs + ");\n"
                "    cpu.cpsr.V = ((~(" + lhs + " ^ " + rhs + ") & (" + lhs +
                " ^ temp)) >> 31) != 0;\n";
    } else if (sub || cmp) {
        // Subtracting immediate zero never borrows; avoid an unsigned >= 0 warning.
        code += "    cpu.cpsr.C = " + (immediate && immediate_value == 0 ?
                 std::string{"true"} : "(" + lhs + " >= " + rhs + ")") + ";\n";
        code += "    cpu.cpsr.V = (((" + lhs + " ^ " + rhs + ") & (" + lhs +
                " ^ temp)) >> 31) != 0;\n";
    } else if (immediate && rotation != 0) {
        // Logical modified immediates carry out the rotated immediate's top bit.
        code += std::string{"    cpu.cpsr.C = "} +
                ((immediate_value & 0x80000000u) != 0 ? "true;\n" : "false;\n");
    }
    // Logical operations preserve V; unshifted registers/zero rotations preserve C.
    // Commit Rd last so flag formulas see original Rn/Rm even when Rd aliases them.
    if (!cmp) {
        code += "    " + reg(rd) + " = temp;\n";
    }
    return code + "}";
}

} // namespace arm11
