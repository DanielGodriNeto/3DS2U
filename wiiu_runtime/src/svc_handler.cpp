#include "svc_handler.hpp"
#include "arm11_types.hpp"
#include "wiiu_memory.hpp"

#include <coreinit/debug.h>
#include <coreinit/event.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>

namespace wiiu {
namespace {
struct Lock {
    OSMutex& mutex;
    explicit Lock(OSMutex& value) : mutex(value) { OSLockMutex(&mutex); }
    ~Lock() { OSUnlockMutex(&mutex); }
};
std::int32_t Signed(std::uint32_t value) { return std::bit_cast<std::int32_t>(value); }
std::int64_t Timeout(std::uint32_t lo, std::uint32_t hi) {
    return std::bit_cast<std::int64_t>(lo | (std::uint64_t{hi} << 32));
}
OSTime Deadline(std::int64_t ns) {
    if (ns < -1) throw std::invalid_argument("Invalid SVC timeout");
    constexpr auto maximum = std::numeric_limits<OSTime>::max();
    if (ns == -1) return maximum;
    const auto frequency = static_cast<std::uint64_t>(OSTimerClockSpeed);
    const auto seconds = static_cast<std::uint64_t>(ns) / 1000000000u;
    const auto remainder = static_cast<std::uint64_t>(ns) % 1000000000u;
    const auto now = OSGetTime();
    const auto available = static_cast<std::uint64_t>(maximum - now);
    if (frequency == 0 || seconds > available / frequency) return maximum;
    const auto ticks = seconds * frequency;
    const auto fraction = remainder * frequency / 1000000000u;
    if (fraction > available - ticks) return maximum;
    return now + static_cast<OSTime>(ticks + fraction);
}
} // namespace

struct SvcHandler::Impl {
    struct Waiter {
        OSEvent event{};
        WaitState state = WaitState::Waiting;
        std::uint32_t handle{}, address{};
        bool address_wait{};
        OSTime deadline{};
        std::uint64_t sequence{};
        Waiter() { OSInitEvent(&event, FALSE, OS_EVENT_MODE_MANUAL); }
    };
    struct Event { bool signaled{}, automatic{}; };
    struct alignas(16) Thread {
        OSThread native{};
        arm11::CPUContext cpu{};
        Impl* owner{};
        void* stack{};
        const char* argv[1]{};
        std::uint32_t handle{};
        bool finished{}, closed{};
        ~Thread() { if (stack != nullptr) MEMFreeToDefaultHeap(stack); }
    };
    struct DeleteThread {
        void operator()(Thread* thread) const noexcept {
            thread->~Thread(); MEMFreeToDefaultHeap(thread);
        }
    };
    WiiUMemory& memory;
    SvcHandler& facade;
    BlockRunner runner;
    void* opaque;
    OSMutex mutex{};
    std::atomic_bool stopping{};
    std::map<std::uint32_t, std::uint32_t> allocations;
    std::map<std::uint32_t, std::unique_ptr<Thread, DeleteThread>> threads;
    std::set<std::uint32_t> arbiters;
    std::map<std::uint32_t, Event> events;
    std::map<const arm11::CPUContext*, std::shared_ptr<Waiter>> waits;
    std::uint32_t next_handle = 0x100u;
    std::uint64_t sequence{};

    Impl(WiiUMemory& mem, SvcHandler& svc, BlockRunner callback, void* data)
        : memory(mem), facade(svc), runner(callback), opaque(data) { OSInitMutex(&mutex); }
    static void Wake(Waiter& waiter, WaitState state) {
        waiter.state = state;
        OSSignalEvent(&waiter.event);
    }
    std::shared_ptr<Waiter> BeginWait(arm11::CPUContext& cpu, std::uint32_t handle,
                                     std::uint32_t address, bool address_wait, std::int64_t ns) {
        auto waiter = std::make_shared<Waiter>();
        waiter->deadline = Deadline(ns); waiter->handle = handle;
        waiter->address = address; waiter->address_wait = address_wait;
        waiter->sequence = ++sequence;
        waits[&cpu] = waiter;
        return waiter;
    }
    unsigned Signal(std::uint32_t handle, std::uint32_t address, bool address_wait, std::int32_t count) {
        unsigned awakened = 0;
        while (count != 0) {
            std::shared_ptr<Waiter> selected;
            for (const auto& [cpu, waiter] : waits) {
                (void)cpu;
                if (waiter->state == WaitState::Waiting && OSGetTime() >= waiter->deadline)
                    Wake(*waiter, WaitState::TimedOut);
                if (waiter->state == WaitState::Waiting && waiter->handle == handle &&
                    waiter->address_wait == address_wait && (!address_wait || waiter->address == address) &&
                    (!selected || waiter->sequence < selected->sequence)) selected = waiter;
            }
            if (!selected) break;
            Wake(*selected, WaitState::Signaled);
            ++awakened;
            if (count > 0) --count;
        }
        return awakened;
    }
    bool Ready(arm11::CPUContext& cpu) {
        Lock lock(mutex);
        const auto it = waits.find(&cpu);
        if (it == waits.end()) return true;
        if (it->second->state == WaitState::Waiting && OSGetTime() >= it->second->deadline)
            Wake(*it->second, WaitState::TimedOut);
        return it->second->state != WaitState::Waiting;
    }
    std::uint32_t ControlMemory(arm11::CPUContext& cpu) {
        const auto operation = cpu.r[0], requested = cpu.r[1], size = cpu.r[3];
        cpu.r[1] = 0;
        if ((operation != 1 && operation != 3) || cpu.r[2] != 0) return kNotSupported;
        if (size == 0 || size > WiiUMemory::kFCRAMSize) return kInvalidArgument;
        const auto rounded = (size + 0xFFFu) & ~0xFFFu;
        if (operation == 1) {
            const auto it = allocations.find(requested);
            if (it == allocations.end() || it->second != rounded) return kInvalidArgument;
            memory.ClearRange(it->first, it->second);
            (void)memory.ReleaseDynamicRange(it->first, it->second);
            allocations.erase(it); return 0;
        }
        if (cpu.r[4] == 0 || cpu.r[4] > 7) return kInvalidArgument;
        const auto floor = (memory.GetImageEnd() + 0xFFFu) & ~0xFFFu;
        constexpr auto end = WiiUMemory::kVirtualBase + WiiUMemory::kFCRAMSize;
        auto address = requested == 0 ? memory.FindFreeRange(rounded) : requested;
        if (requested == 0 && address == 0) return kOutOfResources;
        if ((address & 0xFFFu) != 0 || address < floor || address > end || rounded > end - address)
            return kInvalidArgument;
        if (address > end || rounded > end - address) return kOutOfResources;
        // Allocate bookkeeping before committing the shared memory reservation.
        const auto [record, inserted] = allocations.emplace(address, rounded);
        if (!inserted) return kOutOfResources;
        if (!memory.ReserveDynamicRange(address, rounded)) {
            allocations.erase(record); return kOutOfResources;
        }
        memory.ClearRange(address, rounded);
        cpu.r[1] = address;
        return 0;
    }
    static int ThreadEntry(int, const char** argv) {
        auto& thread = *reinterpret_cast<Thread*>(const_cast<char*>(argv[0]));
        auto& owner = *thread.owner;
        unsigned executed = 0;
        try {
            while (!owner.stopping.load() && !thread.cpu.svc_exit_requested) {
                if (!owner.facade.Ready(thread.cpu)) { owner.facade.Park(thread.cpu); continue; }
                thread.cpu.svc_yield = false;
                if (!owner.runner(owner.opaque, thread.cpu)) break;
                if (++executed % 64 == 0) OSSleepTicks(static_cast<OSTime>(OSMillisecondsToTicks(1)));
            }
        } catch (const std::exception& error) {
            OSReport("[3DS2U SVC] Guest worker stopped: %s\n", error.what());
        }
        {
            Lock lock(owner.mutex);
            thread.finished = true;
            owner.Signal(thread.handle, 0, false, -1);
        }
        // No engine/kernel mutex remains held when terminating the PPC thread.
        OSExitThread(0);
        return 0;
    }
    std::uint32_t CreateThread(arm11::CPUContext& cpu) {
        const auto priority = cpu.r[0], entry = cpu.r[1], argument = cpu.r[2], sp = cpu.r[3];
        const auto processor = Signed(cpu.r[4]); cpu.r[1] = 0;
        if (runner == nullptr) return kNotSupported;
        if (priority > 63 || sp < 16 || (sp & 15u) != 0 || processor < -2 || processor > 1 ||
            ((entry & 1u) == 0 && (entry & 3u) != 0)) return kInvalidArgument;
        (void)memory.GetPointer(sp - 16, 16);
        (void)memory.GetPointer(entry & ~1u, (entry & 1u) != 0 ? 2 : 4);
        if (threads.size() >= 64 || next_handle == 0xFFFFFFFFu) return kOutOfResources;
        auto* storage = MEMAllocFromDefaultHeapEx(static_cast<std::uint32_t>(sizeof(Thread)), 16);
        if (storage == nullptr) return kOutOfResources;
        std::unique_ptr<Thread, DeleteThread> thread{::new (storage) Thread{}};
        constexpr std::uint32_t stack_size = 128u * 1024u;
        thread->stack = MEMAllocFromDefaultHeapEx(stack_size, 16);
        if (thread->stack == nullptr) return kOutOfResources;
        thread->owner = this; thread->handle = next_handle;
        thread->cpu.svc_handler = &facade; thread->cpu.r[0] = argument; thread->cpu.r[13] = sp;
        thread->cpu.BranchExchange(entry);
        thread->argv[0] = reinterpret_cast<const char*>(thread.get());
        auto* raw = thread.get();
        const auto id = next_handle++;
        threads.emplace(id, std::move(thread));
        const auto affinity = processor == 0 ? OS_THREAD_ATTRIB_AFFINITY_CPU0 :
                              processor == 1 ? OS_THREAD_ATTRIB_AFFINITY_CPU1 : OS_THREAD_ATTRIB_AFFINITY_ANY;
        if (!OSCreateThread(&raw->native, ThreadEntry, 1, reinterpret_cast<char*>(raw->argv),
                            static_cast<std::uint8_t*>(raw->stack) + stack_size, stack_size,
                            static_cast<std::int32_t>(priority / 2u), affinity)) {
            threads.erase(id); return kOutOfResources;
        }
        OSResumeThread(&raw->native);
        cpu.r[1] = id;
        return 0;
    }
    std::uint32_t Arbitrate(arm11::CPUContext& cpu) {
        const auto handle = cpu.r[0], address = cpu.r[1], type = cpu.r[2];
        if (!arbiters.contains(handle)) return kInvalidHandle;
        if ((address & 3u) != 0 || type > 4) return kInvalidArgument;
        (void)memory.GetPointer(address, 4);
        if (type == 0) { Signal(handle, address, true, Signed(cpu.r[3])); return 0; }
        const auto ns = type >= 3 ? Timeout(cpu.r[4], cpu.r[5]) : -1;
        const auto deadline = Deadline(ns); // Validate before decrementing guest memory.
        (void)deadline;
        const auto value = memory.Read32(address);
        const auto waiter = BeginWait(cpu, handle, address, true, ns);
        if (Signed(value) >= Signed(cpu.r[3])) Wake(*waiter, WaitState::Signaled);
        else if (type == 2 || type == 4) memory.Write32(address, value - 1u);
        return 0;
    }
    std::uint32_t WaitSynchronization(arm11::CPUContext& cpu) {
        const auto handle = cpu.r[0];
        const auto thread = threads.find(handle);
        const auto event = events.find(handle);
        if ((thread == threads.end() || thread->second->closed) && event == events.end()) return kInvalidHandle;
        auto waiter = BeginWait(cpu, handle, 0, false, Timeout(cpu.r[2], cpu.r[3]));
        if (thread != threads.end() && thread->second->finished) Wake(*waiter, WaitState::Signaled);
        if (event != events.end() && event->second.signaled) {
            Wake(*waiter, WaitState::Signaled);
            if (event->second.automatic) event->second.signaled = false;
        }
        return 0; // MVP contract also returns success when the deadline expires.
    }
    std::uint32_t Dispatch(arm11::CPUContext& cpu, std::uint32_t id) {
        Lock lock(mutex);
        if (stopping.load()) return kNotSupported;
        switch (id) {
        case 0x01: return ControlMemory(cpu);
        case 0x08: return CreateThread(cpu);
        case 0x09: cpu.svc_exit_requested = true; return 0;
        case 0x17:
            if (cpu.r[1] > 1) return kNotSupported;
            if (events.size() >= 256 || next_handle == 0xFFFFFFFFu) return kOutOfResources;
            events.emplace(next_handle, Event{false, cpu.r[1] == 0}); cpu.r[1] = next_handle++; return 0;
        case 0x18: {
            const auto event = events.find(cpu.r[0]);
            if (event == events.end()) return kInvalidHandle;
            event->second.signaled = true;
            const auto awakened = Signal(cpu.r[0], 0, false, event->second.automatic ? 1 : -1);
            if (event->second.automatic && awakened != 0) event->second.signaled = false;
            return 0;
        }
        case 0x19: {
            const auto event = events.find(cpu.r[0]);
            if (event == events.end()) return kInvalidHandle;
            event->second.signaled = false; return 0;
        }
        case 0x21:
            if (arbiters.size() >= 256 || next_handle == 0xFFFFFFFFu) return kOutOfResources;
            arbiters.insert(next_handle); cpu.r[1] = next_handle++; return 0;
        case 0x22: return Arbitrate(cpu);
        case 0x23: {
            const auto handle = cpu.r[0];
            for (const auto& [context, waiter] : waits) {
                (void)context;
                if (waiter->handle == handle && waiter->state == WaitState::Waiting) return kInvalidArgument;
            }
            if (arbiters.erase(handle) != 0 || events.erase(handle) != 0) return 0;
            const auto thread = threads.find(handle);
            if (thread == threads.end() || thread->second->closed) return kInvalidHandle;
            thread->second->closed = true; return 0;
        }
        case 0x24: return WaitSynchronization(cpu);
        default: return kNotSupported;
        }
    }
};

SvcHandler::SvcHandler(WiiUMemory& memory, BlockRunner runner, void* opaque)
    : impl_(std::make_unique<Impl>(memory, *this, runner, opaque)) {}
SvcHandler::~SvcHandler() { Shutdown(); }
void SvcHandler::Execute(arm11::CPUContext& cpu, std::uint32_t id) {
    cpu.svc_yield = true;
    if (cpu.svc_handler == nullptr) { cpu.r[0] = kInvalidHandle; return; }
    try { cpu.r[0] = cpu.svc_handler->impl_->Dispatch(cpu, id); }
    catch (const std::bad_alloc&) { cpu.r[0] = kOutOfResources; }
    catch (const std::exception&) { cpu.r[0] = kInvalidArgument; }
}
bool SvcHandler::Ready(arm11::CPUContext& cpu) { return impl_->Ready(cpu); }
SvcHandler::WaitState SvcHandler::GetWaitState(const arm11::CPUContext& cpu) {
    Lock lock(impl_->mutex);
    const auto it = impl_->waits.find(&cpu);
    return it == impl_->waits.end() ? WaitState::None : it->second->state;
}
void SvcHandler::Park(arm11::CPUContext& cpu) {
    std::shared_ptr<Impl::Waiter> waiter;
    { Lock lock(impl_->mutex); const auto it = impl_->waits.find(&cpu); if (it != impl_->waits.end()) waiter = it->second; }
    // Unlike OSSleepTicks, OSWaitEventWithTimeout takes nanoseconds.
    if (waiter) (void)OSWaitEventWithTimeout(&waiter->event, 1000000);
}
void SvcHandler::Shutdown() noexcept {
    if (!impl_ || impl_->stopping.exchange(true)) return;
    { Lock lock(impl_->mutex); for (auto& [cpu, waiter] : impl_->waits) { (void)cpu; Impl::Wake(*waiter, WaitState::Signaled); } }
    for (auto& [handle, thread] : impl_->threads) {
        (void)handle;
        (void)OSJoinThread(&thread->native, nullptr);
    }
    impl_->threads.clear();
}
} // namespace wiiu
