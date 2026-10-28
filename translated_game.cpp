#include "arm11_types.hpp"
#include "wiiu_engine.hpp"

namespace arm11 {

// Block bodies inherit the current translators' supported subset.
// B/BL/BX and LDM-to-PC return control to the runtime dispatcher.

void block_08000000(arm11::CPUContext& cpu, wiiu::WiiUMemory& memory) {
    (void)cpu;
    (void)memory;
    cpu.r[0] = 0x1E000000u;
    cpu.r[0] = cpu.r[0] | 0xF00000u;
    cpu.r[1] = 0xFF00u;
    {
        const std::uint32_t base = cpu.r[0];
        const std::uint32_t adjusted = base + 0x0u;
        const std::uint32_t value = cpu.r[1];
        memory.Write32(adjusted, value);
    }
    cpu.r[15] = 0x08000010u;
}

void block_08000010(arm11::CPUContext& cpu, wiiu::WiiUMemory& memory) {
    (void)cpu;
    (void)memory;
    cpu.r[15] = 0x8000010; return;
}

} // namespace arm11

// Register translated block entry points with the Wii U engine.
void InitDispatcher(wiiu::WiiUEngine& engine) {
    (void)engine;
    engine.RegisterBlock(0x08000000u, &arm11::block_08000000);
    engine.RegisterBlock(0x08000010u, &arm11::block_08000010);
}
