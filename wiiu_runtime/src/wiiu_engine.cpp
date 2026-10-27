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
    cpu.r[15] = entry_point;

    while (true) {
        const std::uint32_t pc = cpu.r[15];
        const auto it = block_table.find(pc);
        if (it == block_table.end()) {
            OSReport("[3DS2U] Fatal: no translated block at PC 0x%08" PRIX32 "\n", pc);
            break;
        }

        // The block owns guest control flow; the dispatcher never increments PC.
        const BlockFunction block = it->second;
        block(cpu, memory);
    }
}

} // namespace wiiu
