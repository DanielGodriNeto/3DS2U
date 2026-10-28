#include "cfg_builder.hpp"
#include "mem_translator.hpp"
#include "thumb_decoder.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace arm11 {
namespace {

// Direct branches, BX/BLX, and supported LDM-to-PC end the current block.
constexpr bool IsBranch(std::uint32_t opcode) noexcept {
    const auto nibble = (opcode >> 24) & 0xFu;
    return nibble == 0b1010u || nibble == 0b1011u ||
           ((opcode >> 4) & 0x00FFFFFFu) == 0x0012FFF1u ||
           ((opcode >> 4) & 0x00FFFFFFu) == 0x0012FFF3u ||
           MemoryTranslator::IsLoadMultipleToPC({opcode, 0});
}

} // namespace

std::unordered_map<std::uint32_t, BasicBlock>
CFGBuilder::BuildBlocks(const std::vector<Instruction>& instructions) const {
    std::unordered_map<std::uint32_t, BasicBlock> blocks;
    std::unordered_set<std::uint32_t> branch_targets;
    std::array<std::vector<Instruction>, 2> streams;
    for (const auto& inst : instructions) {
        const auto mode = inst.state == InstructionSet::Thumb ? 1u : 0u;
        const auto width = inst.Width();
        if (inst.address % width != 0 || (mode == 1 && inst.opcode > 0xFFFFu) ||
            (!streams[mode].empty() && std::uint64_t{inst.address} <
             std::uint64_t{streams[mode].back().address} + width)) {
            throw std::invalid_argument("Instructions must be aligned and ordered within each execution mode");
        }
        streams[mode].push_back(inst);
        if (inst.state == InstructionSet::Thumb) {
            if (const auto target = ThumbTranslator::BranchTarget(inst.opcode, inst.address)) {
                branch_targets.insert(DispatchKey({*target, InstructionSet::Thumb}));
            }
            continue;
        }
        if ((inst.opcode & 0xFE000000u) == 0xFA000000u) {
            auto offset = inst.opcode & 0x00FFFFFFu;
            if ((offset & 0x00800000u) != 0) offset |= 0xFF000000u;
            const auto target = inst.address + 8u + (offset << 2) + ((inst.opcode >> 23) & 2u);
            branch_targets.insert(DispatchKey({target, InstructionSet::Thumb}));
            continue;
        }
        if ((inst.opcode & 0x0E000000u) == 0x0A000000u &&
            (inst.opcode >> 28) != 0xFu) {
            std::uint32_t offset = inst.opcode & 0x00FFFFFFu;
            if ((offset & 0x00800000u) != 0) {
                offset |= 0xFF000000u;
            }
            branch_targets.insert(inst.address + 8u + (offset << 2));
        }
    }
    for (const auto& stream : streams) {
        BasicBlock* current = nullptr;
        std::uint64_t expected_address = 0;
        for (const auto& inst : stream) {
            const auto key = DispatchKey({inst.address, inst.state});
            const auto width = inst.Width();
            if (current == nullptr || inst.address != expected_address || branch_targets.contains(key)) {
                auto [it, inserted] = blocks.try_emplace(key);
                if (!inserted) {
                    throw std::invalid_argument("Duplicate block dispatch key");
                }
                current = &it->second;
                current->start_address = inst.address;
                current->start = {inst.address, inst.state};
            }

            // Keep the terminating branch in its source block, including final BX.
            if (current->byte_size > std::numeric_limits<std::uint32_t>::max() - width) {
                throw std::overflow_error("Block byte size exceeds metadata capacity");
            }
            current->instructions.push_back(inst);
            current->byte_size += width;
            ++current->instruction_count;
            expected_address = static_cast<std::uint64_t>(inst.address) + width;
            if (inst.state == InstructionSet::Thumb ? ThumbTranslator::IsTerminator(inst.opcode) : IsBranch(inst.opcode)) {
                current->has_indirect_successor =
                    inst.state == InstructionSet::Thumb ?
                        (ThumbTranslator::IsExchange(inst.opcode) || ThumbTranslator::IsPopPC(inst.opcode)) :
                        (MemoryTranslator::IsLoadMultipleToPC(inst) ||
                         ((inst.opcode >> 4) & 0x00FFFFFDu) == 0x0012FFF1u);
                current = nullptr;
            }
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
             << std::setw(8) << blocks.at(address).start_address
             << (blocks.at(address).start.state == InstructionSet::Thumb ? " [Thumb]: " : " [ARM]: ") << std::dec
             << blocks.at(address).instructions.size() << " instruction(s)\n";
        std::cout << line.str();
    }
}

} // namespace arm11
