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

    // Other addressing modes and byte transfers need distinct AOT semantics.
    if (register_offset || !pre_indexed || !add_offset || byte_transfer || writeback) {
        return "// Unhandled Instruction";
    }

    std::ostringstream offset;
    offset << "0x" << std::hex << std::uppercase << offset_12;
    const auto address = "cpu.r[" + std::to_string(rn) + "] + " + offset.str();
    const auto reg = "cpu.r[" + std::to_string(rd) + "]";

    // Read32/Write32 own guest-LE to host-native conversion; never swap here.
    // Memory owns FCRAM, so these calls require its instance rather than a type.
    if (load) {
        return reg + " = memory.Read32(" + address + ");";
    }
    return "memory.Write32(" + address + ", " + reg + ");";
}

} // namespace arm11
