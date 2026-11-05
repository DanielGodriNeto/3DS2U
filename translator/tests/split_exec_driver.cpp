// Host driver for translated output (plan tests 21-22). It links the generated files, runs blocks
// from the image entry until no block is registered or a block branches to itself, then prints the
// final state. Exit 1 when no register r0-r15 changed. Output is deterministic, so the unsplit and
// split dumps can be compared byte for byte.

#include "arm11_types.hpp"
#include "svc_handler.hpp"
#include "wiiu_engine.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

void InitDispatcher(wiiu::WiiUEngine& engine);

namespace {

constexpr int kMaxBlocks = 256;
using RegisterFile = std::array<std::uint32_t, 16>;

RegisterFile Snapshot(const arm11::CPUContext& cpu) {
    RegisterFile regs{};
    for (unsigned i = 0; i < regs.size(); ++i) regs[i] = cpu.r[i];
    return regs;
}

// Runs blocks while the PC has a registered block; stops on a self-branch or at the block limit.
int RunBlocks(wiiu::WiiUEngine& engine) {
    auto& cpu = engine.GetCPU();
    int blocks = 0;
    while (blocks < kMaxBlocks) {
        const std::uint32_t pc = cpu.r[15];
        const auto block = engine.FindBlock(pc | (cpu.thumb ? 1u : 0u));
        if (block == nullptr) break;
        block(cpu, engine.GetMemory());
        ++blocks;
        if (cpu.r[15] == pc) break;
    }
    return blocks;
}

void PrintState(const arm11::CPUContext& cpu, int blocks) {
    std::printf("blocks=%d\n", blocks);
    for (unsigned i = 0; i < 16; ++i) std::printf("r%u=%08x\n", i, static_cast<unsigned>(cpu.r[i]));
    std::printf("cpsr N=%d Z=%d C=%d V=%d thumb=%d\n", static_cast<int>(cpu.cpsr.N),
                static_cast<int>(cpu.cpsr.Z), static_cast<int>(cpu.cpsr.C),
                static_cast<int>(cpu.cpsr.V), static_cast<int>(cpu.thumb));
    for (const auto id : wiiu::SvcHandler::calls) std::printf("svc=%x\n", static_cast<unsigned>(id));
}

} // namespace

int main() {
    wiiu::WiiUEngine engine;
    InitDispatcher(engine);
    const RegisterFile before = Snapshot(engine.GetCPU());
    const int blocks = RunBlocks(engine);
    PrintState(engine.GetCPU(), blocks);
    return Snapshot(engine.GetCPU()) == before ? 1 : 0;
}
