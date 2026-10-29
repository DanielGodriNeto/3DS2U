#include "svc_handler.hpp"
#include "wiiu_engine.hpp"
#include "wiiu_memory.hpp"
#include "pica200.hpp"
#include <coreinit/thread.h>
#include <vpad/input.h>
#include <atomic>
#include <array>
#include <iostream>
#include <malloc.h>
#include <stdexcept>

#define InitDispatcher InitArmDispatcher
#include "svc_arm.cpp"
#undef InitDispatcher
#define InitDispatcher InitThumbDispatcher
#include "svc_thumb.cpp"
#undef InitDispatcher
#define InitDispatcher InitConditionalDispatcher
#include "svc_conditional.cpp"
#undef InitDispatcher

namespace {
void Check(bool value) { if (!value) throw std::runtime_error("SVC regression"); }
void Call(arm11::CPUContext& cpu, std::uint32_t svc, std::array<std::uint32_t, 6> args) {
    for (unsigned i = 0; i < 6; ++i) cpu.r[i] = args[i];
    wiiu::SvcHandler::Execute(cpu, svc);
}
struct Worker {
    std::atomic_bool ran{}, wait_mode{}, queued{};
    std::atomic<std::uint32_t> entry{}, sp{}, argument{}, arbiter{}, address{};
};
bool RunWorker(void* data, arm11::CPUContext& cpu) {
    auto& worker = *static_cast<Worker*>(data);
    if (!worker.ran.exchange(true)) {
        worker.entry.store(cpu.r[15]); worker.sp.store(cpu.r[13]); worker.argument.store(cpu.r[0]);
        if (worker.wait_mode.load()) {
            cpu.r[0] = worker.arbiter.load(); cpu.r[1] = worker.address.load(); cpu.r[2] = 1; cpu.r[3] = 1;
            wiiu::SvcHandler::Execute(cpu, 0x22);
            worker.queued.store(true);
            return true;
        }
    }
    wiiu::SvcHandler::Execute(cpu, 9);
    return !cpu.svc_exit_requested;
}
} // namespace

// Production memory/SVC/engine code; only platform allocation/input/GPU APIs differ.
void* MEMAllocFromDefaultHeapEx(std::uint32_t size, std::int32_t alignment) { return _aligned_malloc(size, alignment); }
void MEMFreeToDefaultHeap(void* pointer) { _aligned_free(pointer); }
void OSReport(const char*, ...) {}
void wiiu::Pica200::WriteMMIO(std::uint32_t, std::uint32_t, std::uint8_t) {}
std::int32_t VPADRead(VPADChan, VPADStatus*, std::uint32_t, VPADReadError* error) { *error = VPAD_READ_NO_SAMPLES; return 0; }
void VPADGetTPCalibratedPointEx(VPADChan, VPADTouchPadResolution, VPADTouchData*, const VPADTouchData*) {}

int main() {
    using Kernel = wiiu::SvcHandler;
    wiiu::WiiUMemory memory;
    memory.ReserveImageRange(0x08000000u, 0x10000);
    Worker worker;
    Kernel kernel(memory, RunWorker, &worker);
    arm11::CPUContext cpu{}, waiter{}, second{}, signaler{};
    cpu.svc_handler = waiter.svc_handler = second.svc_handler = signaler.svc_handler = &kernel;
    Call(cpu, 1, {3, 0, 0, 1, 3, 0});
    Check(cpu.r[0] == 0 && cpu.r[1] == 0x08010000u);
    const auto first = cpu.r[1];
    Call(cpu, 1, {3, 0, 0, 4097, 3, 0});
    Check(cpu.r[0] == 0 && cpu.r[1] == first + 4096);
    const auto large = cpu.r[1];
    Call(cpu, 1, {3, first, 0, 4096, 3, 0}); Check(cpu.r[0] != 0);
    Call(cpu, 1, {1, first, 0, 1, 0, 0}); Check(cpu.r[0] == 0);
    Call(cpu, 1, {1, first, 0, 1, 0, 0}); Check(cpu.r[0] != 0);
    Call(cpu, 1, {3, 0, 0, 4096, 3, 0}); Check(cpu.r[0] == 0 && cpu.r[1] == first);
    Call(cpu, 1, {3, 0x0FFFF000u, 0, 8192, 3, 0}); Check(cpu.r[0] != 0);
    Call(cpu, 1, {3, 0x08000000u, 0, 4096, 3, 0}); Check(cpu.r[0] != 0);
    Call(cpu, 1, {3, 0, 0, 0, 3, 0}); Check(cpu.r[0] != 0);
    Check(memory.Read32(first) == 0 && (large & 0xFFFu) == 0);
    // CRO and kernel allocations share the same reservation map.
    constexpr std::uint32_t module_range = 0x08013000u;
    Check(memory.ReserveDynamicRange(module_range, 0x2000));
    Call(cpu, 1, {3, module_range, 0, 4096, 3, 0}); Check(cpu.r[0] == Kernel::kOutOfResources);
    Call(cpu, 1, {3, 0, 0, 4096, 3, 0}); Check(cpu.r[0] == 0 && cpu.r[1] == module_range + 0x2000);
    Call(cpu, 1, {1, module_range + 0x2000, 0, 4096, 0, 0}); Check(cpu.r[0] == 0);
    Check(memory.ReleaseDynamicRange(module_range, 0x2000));

    Call(cpu, 0x21, {}); Check(cpu.r[0] == 0); const auto arbiter = cpu.r[1];
    constexpr std::uint32_t address = 0x08000800u;
    memory.Write32(address, 0);
    Call(waiter, 0x22, {arbiter, address, 1, 1, 0, 0});
    Check(waiter.r[0] == 0 && !kernel.Ready(waiter) && kernel.GetWaitState(waiter) == Kernel::WaitState::Waiting);
    kernel.Park(waiter); // Bounded native event wait must not hold the kernel mutex.
    Call(second, 0x22, {arbiter, address, 1, 1, 0, 0});
    Call(signaler, 0x22, {arbiter, address, 0, 1, 0, 0});
    Check(kernel.Ready(waiter) && !kernel.Ready(second));
    Call(signaler, 0x22, {arbiter, address, 0, 0xFFFFFFFFu, 0, 0});
    Check(kernel.Ready(second));
    Call(waiter, 0x22, {arbiter, address, 2, 1, 0, 0}); Check(memory.Read32(address) == 0xFFFFFFFFu);
    Call(signaler, 0x22, {arbiter, address, 0, 1, 0, 0}); Check(kernel.Ready(waiter));
    Call(waiter, 0x22, {arbiter, address, 3, 1, 0, 0});
    Check(kernel.Ready(waiter) && kernel.GetWaitState(waiter) == Kernel::WaitState::TimedOut && waiter.r[0] == 0);
    Call(signaler, 0x22, {0xDEAD, address, 0, 1, 0, 0}); Check(signaler.r[0] == Kernel::kInvalidHandle);

    Call(cpu, 0x17, {0, 0, 0, 0, 0, 0}); const auto event = cpu.r[1];
    Call(waiter, 0x24, {event, 0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0}); Check(!kernel.Ready(waiter));
    Call(signaler, 0x18, {event, 0, 0, 0, 0, 0}); Check(kernel.Ready(waiter));
    Call(waiter, 0x24, {event, 0, 0, 0, 0, 0});
    Check(kernel.Ready(waiter) && kernel.GetWaitState(waiter) == Kernel::WaitState::TimedOut);
    Call(signaler, 0x18, {event, 0, 0, 0, 0, 0});
    Call(waiter, 0x24, {event, 0, 0, 0, 0, 0});
    Check(kernel.GetWaitState(waiter) == Kernel::WaitState::Signaled);
    Call(cpu, 0x27, {}); Check(cpu.r[0] == Kernel::kNotSupported);

    // Raw CreateThread ABI: priority, entry, argument, guest SP, processor ID.
    Call(cpu, 8, {48, 0x08002000u, 0x1234, large + 8192, 0, 0});
    Check(cpu.r[0] == 0); const auto thread = cpu.r[1];
    Call(waiter, 0x24, {thread, 0, 1000000000u, 0, 0, 0});
    for (unsigned i = 0; i < 2000 && !kernel.Ready(waiter); ++i) OSSleepTicks(1000);
    Check(kernel.Ready(waiter) && kernel.GetWaitState(waiter) == Kernel::WaitState::Signaled);
    Check(worker.ran.load() && worker.entry.load() == 0x08002000u && worker.argument.load() == 0x1234u);
    Check(worker.sp.load() == large + 8192 && svc_test_platform::priority == 24 && svc_test_platform::stack_aligned);
    Call(cpu, 8, {48, 0x08002000u, 0, large + 1, 0, 0}); Check(cpu.r[0] == Kernel::kInvalidArgument);
    svc_test_platform::fail_create = true;
    Call(cpu, 8, {48, 0x08002000u, 0, large + 8192, 0, 0}); Check(cpu.r[0] == Kernel::kOutOfResources);
    svc_test_platform::fail_create = false;
    worker.ran.store(false); worker.wait_mode.store(true); worker.arbiter.store(arbiter); worker.address.store(address);
    memory.Write32(address, 0);
    Call(cpu, 8, {48, 0x08002000u, 0x5678u, large + 8192, 0, 0});
    Check(cpu.r[0] == 0); const auto waiting_thread = cpu.r[1];
    for (unsigned i = 0; i < 2000 && !worker.queued.load(); ++i) OSSleepTicks(1000);
    Check(worker.queued.load());
    Call(signaler, 0x22, {arbiter, address, 0, 1, 0, 0});
    Call(waiter, 0x24, {waiting_thread, 0, 1000000000u, 0, 0, 0});
    for (unsigned i = 0; i < 2000 && !kernel.Ready(waiter); ++i) OSSleepTicks(1000);
    Check(kernel.GetWaitState(waiter) == Kernel::WaitState::Signaled);
    worker.ran.store(false); worker.queued.store(false);
    Call(cpu, 8, {48, 0x08002000u, 0, large + 8192, 0, 0}); Check(cpu.r[0] == 0);
    for (unsigned i = 0; i < 2000 && !worker.queued.load(); ++i) OSSleepTicks(1000);
    Check(worker.queued.load()); // Shutdown must wake/join a parked native worker.
    kernel.Shutdown();
    Check(svc_test_platform::created.load() == svc_test_platform::resumed.load() && svc_test_platform::exited.load() == 3);

    const auto emitted = [](auto initialize, std::uint32_t pc, bool thumb) {
        wiiu::WiiUEngine engine;
        initialize(engine);
        auto& state = engine.GetCPU(); state.thumb = thumb;
        state.r[0] = 3; state.r[3] = 1; state.r[4] = 3;
        engine.Run(pc);
        Check(state.r[0] == 0 && state.r[1] >= 0x08001000u && state.r[15] == pc + (thumb ? 2u : 4u));
        engine.Run(state.r[15]);
        Check(state.svc_exit_requested && !engine.IsHalted());
    };
    emitted(InitArmDispatcher, 0x08000000u, false);
    emitted(InitThumbDispatcher, 0x08000010u, true);
    {
        wiiu::WiiUEngine engine; InitConditionalDispatcher(engine);
        auto& state = engine.GetCPU(); state.cpsr.Z = true; state.r[0] = 3;
        engine.Run(0x08000020u);
        Check(state.r[0] == 3 && state.r[15] == 0x08000024u);
    }
    {
        wiiu::WiiUEngine engine; InitConditionalDispatcher(engine);
        auto& state = engine.GetCPU(); state.cpsr.Z = false;
        state.r[0] = 3; state.r[3] = 1; state.r[4] = 3;
        engine.Run(0x08000020u);
        Check(state.r[0] == 0 && state.r[1] >= 0x08001000u && state.r[15] == 0x08000024u);
    }
    std::cout << "SVC heap, arbitration, events, native workers and emitted ARM/Thumb execution passed.\n";
}
