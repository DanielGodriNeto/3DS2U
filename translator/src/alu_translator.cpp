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
    const bool set_flags = ((raw >> 20) & 1u) != 0;
    const auto rn = (raw >> 16) & 0xFu;
    const auto rd = (raw >> 12) & 0xFu;
    const auto operand2 = raw & 0xFFFu;
    const auto rm = operand2 & 0xFu;

    // Operand2 is deliberately treated as an unshifted register at this stage.
    const auto reg = [](std::uint32_t index) {
        return "cpu.r[" + std::to_string(index) + "]";
    };

    const bool add = opcode == 0b0100u;
    const bool sub = opcode == 0b0010u;
    const bool cmp = opcode == 0b1010u;
    const bool mov = opcode == 0b1101u;
    if (!add && !sub && !cmp && !mov) {
        return "// Unhandled Instruction";
    }

    const auto lhs = reg(rn);
    const auto rhs = reg(rm);
    const auto expression = mov ? rhs : lhs + (add ? " + " : " - ") + rhs;
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
        code += "    cpu.cpsr.C = (" + lhs + " >= " + rhs + ");\n"
                "    cpu.cpsr.V = (((" + lhs + " ^ " + rhs + ") & (" + lhs +
                " ^ temp)) >> 31) != 0;\n";
    }
    // MOVS preserves C until shifter carry is implemented, and always preserves V.
    // Commit Rd last so flag formulas see original Rn/Rm even when Rd aliases them.
    if (!cmp) {
        code += "    " + reg(rd) + " = temp;\n";
    }
    return code + "}";
}

} // namespace arm11
