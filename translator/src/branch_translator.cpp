#include "branch_translator.hpp"

#include <iomanip>
#include <sstream>

namespace arm11 {

std::string BranchTranslator::TranslateInstruction(
    const Instruction& inst, std::uint32_t current_pc) const {
    const std::uint32_t raw = inst.opcode;
    if ((raw & 0xFE000000u) == 0xFA000000u) {
        auto offset = raw & 0x00FFFFFFu;
        if ((offset & 0x00800000u) != 0) offset |= 0xFF000000u;
        const auto target = current_pc + 8u + (offset << 2) + ((raw >> 23) & 2u);
        return "cpu.r[14] = " + std::to_string(current_pc + 4u) +
               "u;\ncpu.BranchExchange(" + std::to_string(target | 1u) + "u);\nreturn;";
    }
    // The emitter supplies CPSR guards; 0xF belongs to other ARM encodings.
    if ((raw >> 28) == 0xFu) {
        return "// Unhandled Branch";
    }

    const auto opcode = (raw >> 24) & 0xFu;
    if (opcode == 0b1010u || opcode == 0b1011u) {
        std::uint32_t offset = raw & 0x00FFFFFFu;
        if ((offset & 0x00800000u) != 0) {
            offset |= 0xFF000000u;
        }
        // Unsigned arithmetic preserves 32-bit wrapping without shifting a
        // negative signed integer when resolving backward ARM branches.
        const std::uint32_t target_pc = current_pc + 8u + (offset << 2);
        std::ostringstream code;
        code << std::hex << std::uppercase;
        if (opcode == 0b1011u) {
            // LR points to the instruction after BL, without the pipeline bias.
            const std::uint32_t return_pc = current_pc + 4u;
            code << "cpu.r[14] = 0x" << return_pc << ";\n";
        }
        code << "cpu.r[15] = 0x" << target_pc
             << (opcode == 0b1011u ? ";\nreturn;" : "; return;");
        return code.str();
    }

    if (((raw >> 4) & 0x00FFFFFFu) == 0x0012FFF1u ||
        ((raw >> 4) & 0x00FFFFFFu) == 0x0012FFF3u) {
        const auto rm = raw & 0xFu;
        const auto target = rm == 15 ? std::to_string(current_pc + 8u) + "u" :
                                      "cpu.r[" + std::to_string(rm) + "]";
        if ((raw & 0x20u) == 0) return "cpu.BranchExchange(" + target + "); return;";
        return "{\n    const std::uint32_t target = " + target + ";\n"
               "    cpu.r[14] = " + std::to_string(current_pc + 4u) + "u;\n"
               "    cpu.BranchExchange(target);\n    return;\n}";
    }

    return "// Unhandled Branch";
}

} // namespace arm11
