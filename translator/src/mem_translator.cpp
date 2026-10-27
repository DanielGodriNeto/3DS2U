#include "mem_translator.hpp"

#include <cstdint>
#include <iomanip>
#include <sstream>

namespace arm11 {

std::string MemoryTranslator::TranslateInstruction(const Instruction& inst) const {
    const std::uint32_t raw = inst.opcode;
    if (((raw >> 26) & 0b11u) != 0b01u) {
        return "// Unhandled Instruction";
    }

    const bool register_offset = ((raw >> 25) & 1u) != 0;
    const bool pre_indexed = ((raw >> 24) & 1u) != 0;
    const bool add_offset = ((raw >> 23) & 1u) != 0;
    const bool byte_transfer = ((raw >> 22) & 1u) != 0;
    const bool writeback = ((raw >> 21) & 1u) != 0;
    const bool load = ((raw >> 20) & 1u) != 0;
    const auto rn = (raw >> 16) & 0xFu;
    const auto rd = (raw >> 12) & 0xFu;
    const auto offset_12 = raw & 0xFFFu;

    // Register-offset and byte transfers require separate address/data decoding.
    if (register_offset || byte_transfer) {
        return "// Unhandled Instruction";
    }

    std::ostringstream offset;
    offset << "0x" << std::hex << std::uppercase << offset_12;
    const auto base_reg = "cpu.r[" + std::to_string(rn) + "]";
    const auto reg = "cpu.r[" + std::to_string(rd) + "]";

    // Snapshot the original base so a load into Rd cannot change post-index math.
    // Unsigned arithmetic preserves 32-bit guest-address wrapping in both modes.
    std::string code = "{\n    const std::uint32_t base = " + base_reg + ";\n"
                       "    const std::uint32_t adjusted = base " +
                       (add_offset ? "+ " : "- ") + offset.str() + "u;\n";
    if (!load) {
        // STR uses the original source value even when Rn is updated first.
        code += "    const std::uint32_t value = " + reg + ";\n";
    }
    if (pre_indexed && writeback) {
        code += "    " + base_reg + " = adjusted;\n";
    }

    // The block's memory parameter references engine-owned WiiUMemory. Its
    // Read32/Write32 methods perform all guest-LE/PowerPC-BE conversion.
    const std::string address = pre_indexed ? "adjusted" : "base";
    if (load) {
        code += "    " + reg + " = memory.Read32(" + address + ");\n";
    } else {
        code += "    memory.Write32(" + address + ", value);\n";
    }
    if (!pre_indexed) {
        code += "    " + base_reg + " = adjusted;\n";
    }
    return code + "}";
}

} // namespace arm11
