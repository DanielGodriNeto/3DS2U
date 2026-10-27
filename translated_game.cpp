#include "arm11_types.hpp"
#include <cstdint>
#include <unordered_map>

namespace arm11 {

using BlockFunction = void (*)(arm11::CPUContext&, arm11::Memory&);
using BlockDispatcher = std::unordered_map<std::uint32_t, BlockFunction>;

// Block bodies inherit the current translators' supported subset.
// Branch execution and dispatcher scheduling are implemented separately.

void block_08000000(arm11::CPUContext& cpu, arm11::Memory& memory) {
    (void)cpu;
    (void)memory;
    cpu.r[1] = cpu.r[2];
    cpu.r[3] = cpu.r[1] + cpu.r[2];
    cpu.r[1] = memory.Read32(cpu.r[0] + 0x10);
    memory.Write32(cpu.r[0] + 0x20, cpu.r[1]);
    // Unhandled Instruction
}

// Replace the dispatch table with the emitted block entry points.
void InitDispatcher(BlockDispatcher& dispatcher) {
    dispatcher.clear();
    dispatcher.reserve(1);
    dispatcher.emplace(0x08000000u, &block_08000000);
}

} // namespace arm11
