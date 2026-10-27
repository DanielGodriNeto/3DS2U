#include "arm11_types.hpp"
#include "wiiu_engine.hpp"

namespace arm11 {

// Block bodies inherit the current translators' supported subset.
// Branch execution and dispatcher scheduling are implemented separately.

void block_08000000(arm11::CPUContext& cpu, wiiu::WiiUMemory& memory) {
    (void)cpu;
    (void)memory;
    cpu.r[1] = cpu.r[2];
    cpu.r[3] = cpu.r[1] + cpu.r[2];
    cpu.r[1] = memory.Read32(cpu.r[0] + 0x10);
    memory.Write32(cpu.r[0] + 0x20, cpu.r[1]);
    // Unhandled Instruction
}

} // namespace arm11

// Register translated block entry points with the Wii U engine.
void InitDispatcher(wiiu::WiiUEngine& engine) {
    engine.RegisterBlock(0x08000000u, &arm11::block_08000000);
}
