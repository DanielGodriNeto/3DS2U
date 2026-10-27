#include "arm11_types.hpp"
#include "wiiu_engine.hpp"

namespace arm11 {

// Block bodies inherit the current translators' supported subset.
// B/BX update the guest PC and return to the runtime dispatcher.

void block_08000000(arm11::CPUContext& cpu, wiiu::WiiUMemory& memory) {
    (void)cpu;
    (void)memory;
    cpu.r[1] = cpu.r[2];
    cpu.r[3] = cpu.r[1] + cpu.r[2];
    cpu.r[15] = 0x08000008u;
}

void block_08000008(arm11::CPUContext& cpu, wiiu::WiiUMemory& memory) {
    (void)cpu;
    (void)memory;
    cpu.r[15] = 0x8000008; return;
}

} // namespace arm11

// Register translated block entry points with the Wii U engine.
void InitDispatcher(wiiu::WiiUEngine& engine) {
    (void)engine;
    engine.RegisterBlock(0x08000000u, &arm11::block_08000000);
    engine.RegisterBlock(0x08000008u, &arm11::block_08000008);
}
