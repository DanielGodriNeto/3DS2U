#include "cfg_builder.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace arm11 {
namespace {

// Keep B/BL and BX as block terminators, including conditional forms.
constexpr bool IsBranch(std::uint32_t opcode) noexcept {
    const auto nibble = (opcode >> 24) & 0xFu;
    return nibble == 0b1010u || nibble == 0b1011u ||
           ((opcode >> 4) & 0x00FFFFFFu) == 0x0012FFF1u;
}

} // namespace

std::unordered_map<std::uint32_t, BasicBlock>
CFGBuilder::BuildBlocks(const std::vector<Instruction>& instructions) const {
    std::unordered_map<std::uint32_t, BasicBlock> blocks;
    std::unordered_set<std::uint32_t> branch_targets;
    for (const auto& inst : instructions) {
        if ((inst.opcode & 0xFF000000u) == 0xEA000000u) {
            std::uint32_t offset = inst.opcode & 0x00FFFFFFu;
            if ((offset & 0x00800000u) != 0) {
                offset |= 0xFF000000u;
            }
            branch_targets.insert(inst.address + 8u + (offset << 2));
        }
    }
    BasicBlock* current = nullptr;
    std::uint64_t expected_address = 0;

    for (const auto& inst : instructions) {
        if (inst.address % 4 != 0 ||
            (expected_address != 0 && inst.address < expected_address)) {
            throw std::invalid_argument("ARM instructions must be aligned and strictly ordered");
        }

        if (current == nullptr || inst.address != expected_address ||
            branch_targets.contains(inst.address)) {
            auto [it, inserted] = blocks.try_emplace(inst.address);
            if (!inserted) {
                throw std::invalid_argument("Duplicate ARM block address");
            }
            current = &it->second;
            current->start_address = inst.address;
            current->start = {inst.address, InstructionSet::ARM};
        }

        // Keep the terminating branch in its source block, including final BX.
        if (current->byte_size > std::numeric_limits<std::uint32_t>::max() - 4u) {
            throw std::overflow_error("ARM block byte size exceeds metadata capacity");
        }
        current->instructions.push_back(inst);
        current->byte_size += 4;
        ++current->instruction_count;
        expected_address = static_cast<std::uint64_t>(inst.address) + 4;
        if (IsBranch(inst.opcode)) {
            current = nullptr;
        }
    }
    return blocks;
}

void CFGBuilder::PrintBlocks(
    const std::unordered_map<std::uint32_t, BasicBlock>& blocks) const {
    std::vector<std::uint32_t> addresses;
    addresses.reserve(blocks.size());
    for (const auto& [address, block] : blocks) {
        addresses.push_back(address);
    }
    std::sort(addresses.begin(), addresses.end());

    for (const auto address : addresses) {
        // Local formatting preserves the caller's std::cout state.
        std::ostringstream line;
        line << "Block 0x" << std::hex << std::uppercase << std::setfill('0')
             << std::setw(8) << address << ": " << std::dec
             << blocks.at(address).instructions.size() << " instruction(s)\n";
        std::cout << line.str();
    }
}

} // namespace arm11
