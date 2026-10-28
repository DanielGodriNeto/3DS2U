#include "decoder.hpp"

#include <cstddef>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace arm11 {

std::vector<Instruction>
Decoder::DecodeBuffer(std::span<const std::uint8_t> buffer,
                      std::uint32_t base_address) const {
    constexpr std::size_t instruction_size = 4;
    if (base_address % instruction_size != 0 ||
        buffer.size() % instruction_size != 0) {
        throw std::invalid_argument("ARM code requires aligned, complete 4-byte instructions");
    }

    // Check the final byte before narrowing offsets to guest addresses.
    constexpr auto max_address = std::numeric_limits<std::uint32_t>::max();
    if (!buffer.empty() && buffer.size() - 1 > max_address - base_address) {
        throw std::out_of_range("ARM code exceeds the 32-bit guest address space");
    }

    std::vector<Instruction> instructions;
    instructions.reserve(buffer.size() / instruction_size);
    for (std::size_t offset = 0; offset < buffer.size(); offset += instruction_size) {
        // Assemble guest little-endian bytes without alignment or aliasing risks.
        // This also keeps instruction fetching correct on a big-endian host.
        const std::uint32_t opcode =
            static_cast<std::uint32_t>(buffer[offset]) |
            (static_cast<std::uint32_t>(buffer[offset + 1]) << 8) |
            (static_cast<std::uint32_t>(buffer[offset + 2]) << 16) |
            (static_cast<std::uint32_t>(buffer[offset + 3]) << 24);
        instructions.push_back({opcode, base_address + static_cast<std::uint32_t>(offset)});
    }
    return instructions;
}

void Decoder::PrintInstruction(const Instruction& inst) const {
    // Format locally so diagnostic output does not change std::cout's flags.
    std::ostringstream line;
    line << "0x" << std::hex << std::uppercase << std::setfill('0')
         << std::setw(8) << inst.address << ": 0x"
         << std::setw(inst.state == InstructionSet::Thumb ? 4 : 8) << inst.opcode << '\n';
    std::cout << line.str();
}

} // namespace arm11
