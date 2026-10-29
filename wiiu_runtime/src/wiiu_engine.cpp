#include "wiiu_engine.hpp"

#include <coreinit/debug.h>

#include <cinttypes>
#include <stdexcept>

namespace wiiu {

WiiUEngine::WiiUEngine(bool is_new_3ds)
    : hid_manager(is_new_3ds), memory(is_new_3ds), svc_handler_(memory, RunWorkerBlock, this) {
    OSInitMutex(&execution_mutex_);
    memory.SetHIDManager(hid_manager);
    memory.SetAudioDriver(&audio_);
    cpu.svc_handler = &svc_handler_;
}

WiiUEngine::~WiiUEngine() { svc_handler_.Shutdown(); memory.SetAudioDriver(nullptr); }

void WiiUEngine::RegisterBlock(std::uint32_t address, BlockFunction block_func) {
    if (execution_started_) throw std::logic_error("Register all guest blocks before execution");
    if (block_func == nullptr) {
        throw std::invalid_argument("Cannot register a null translated block");
    }
    block_table.insert_or_assign(address, block_func);
}

bool WiiUEngine::RunWorkerBlock(void* engine, arm11::CPUContext& context) {
    return static_cast<WiiUEngine*>(engine)->DispatchOne(context);
}

bool WiiUEngine::DispatchOne(arm11::CPUContext& context) {
    OSLockMutex(&execution_mutex_);
    const auto unlock = [&] { memory.SetGPUAccessAllowed(true); OSUnlockMutex(&execution_mutex_); };
    try {
        if (!svc_handler_.Ready(context)) { unlock(); return true; }
        const auto key = arm11::DispatchKey({context.r[15], context.thumb ? arm11::InstructionSet::Thumb : arm11::InstructionSet::ARM});
        const auto it = block_table.find(key);
        if (it == block_table.end()) {
            OSReport("[3DS2U] Fatal: no translated block at PC 0x%08" PRIX32 "\n", context.r[15]);
            if (&context == &cpu) halted_ = true;
            unlock(); return false;
        }
        memory.SetGPUAccessAllowed(&context == &cpu);
        it->second(context, memory);
        unlock();
        return !context.svc_exit_requested;
    } catch (...) { unlock(); throw; }
}

void WiiUEngine::Run(std::uint32_t entry_point) {
    execution_started_ = true;
    OSLockMutex(&execution_mutex_);
    try { hid_manager.Update(); }
    catch (...) { OSUnlockMutex(&execution_mutex_); throw; }
    OSUnlockMutex(&execution_mutex_);
    audio_.Pump();
    if ((entry_point & 1u) != 0) cpu.BranchExchange(entry_point);
    else cpu.r[15] = entry_point;
    halted_ = false;
    cpu.svc_yield = false;
    if (cpu.svc_exit_requested || !svc_handler_.Ready(cpu)) return;

    constexpr std::uint32_t blocks_per_slice = 4096;
    for (std::uint32_t executed = 0; executed < blocks_per_slice; ++executed) {
        if (!DispatchOne(cpu)) break;
        if (cpu.svc_yield || memory.ConsumeFrameRequest()) break;
    }
}

} // namespace wiiu
