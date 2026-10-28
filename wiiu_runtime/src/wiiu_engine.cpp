#include "wiiu_engine.hpp"

#include <coreinit/debug.h>

#include <cinttypes>
#include <stdexcept>

namespace wiiu {

void WiiUEngine::RegisterBlock(std::uint32_t address, BlockFunction block_func) {
    if (block_func == nullptr) {
        throw std::invalid_argument("Cannot register a null translated block");
    }
    block_table.insert_or_assign(address, block_func);
}

void WiiUEngine::Run(std::uint32_t entry_point) {
    if ((entry_point & 1u) != 0) cpu.BranchExchange(entry_point);
    else cpu.r[15] = entry_point;
    halted_ = false;

    constexpr std::uint32_t blocks_per_slice = 4096;
    for (std::uint32_t executed = 0; executed < blocks_per_slice; ++executed) {
        const std::uint32_t pc = cpu.r[15];
        const auto key = arm11::DispatchKey({pc, cpu.thumb ? arm11::InstructionSet::Thumb : arm11::InstructionSet::ARM});
        const auto it = block_table.find(key);
        if (it == block_table.end()) {
            OSReport("[3DS2U] Fatal: no translated %s block at PC 0x%08" PRIX32 "\n",
                     cpu.thumb ? "Thumb" : "ARM", pc);
            halted_ = true;
            break;
        }

        // The block owns guest control flow; the dispatcher never increments PC.
        const BlockFunction block = it->second;
        block(cpu, memory);
    }
}

} // namespace wiiu
