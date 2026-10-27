#include "branch_translator.hpp"

#include <iomanip>
#include <sstream>

namespace arm11 {

std::string BranchTranslator::TranslateInstruction(
    const Instruction& inst, std::uint32_t current_pc) const {
    const std::uint32_t raw = inst.opcode;
    // The emitter supplies CPSR guards; 0xF belongs to other ARM encodings.
    if ((raw >> 28) == 0xFu) {
        return "// Unhandled Branch";
    }

    if (((raw >> 24) & 0xFu) == 0b1010u) {
        std::uint32_t offset = raw & 0x00FFFFFFu;
        if ((offset & 0x00800000u) != 0) {
            offset |= 0xFF000000u;
        }
        // Unsigned arithmetic preserves 32-bit wrapping without shifting a
        // negative signed integer when resolving backward ARM branches.
        const std::uint32_t target_pc = current_pc + 8u + (offset << 2);
        std::ostringstream code;
        code << "cpu.r[15] = 0x" << std::hex << std::uppercase << target_pc
             << "; return;";
        return code.str();
    }

    if (((raw >> 4) & 0x00FFFFFFu) == 0x0012FFF1u) {
        const auto rm = raw & 0xFu;
        return "cpu.r[15] = cpu.r[" + std::to_string(rm) + "]; return;";
    }

    return "// Unhandled Branch";
}

} // namespace arm11
