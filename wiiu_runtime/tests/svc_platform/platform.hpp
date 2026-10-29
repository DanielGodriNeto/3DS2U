#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

using BOOL = int;
inline constexpr BOOL FALSE = 0, TRUE = 1;
using OSTime = std::int64_t;
inline constexpr std::uint64_t OSTimerClockSpeed = 1000000;
#define OSMillisecondsToTicks(value) (static_cast<std::uint64_t>(value) * 1000u)
inline OSTime OSGetTime() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct OSMutex { std::recursive_mutex value; };
inline void OSInitMutex(OSMutex*) {}
inline void OSLockMutex(OSMutex* mutex) { mutex->value.lock(); }
inline void OSUnlockMutex(OSMutex* mutex) { mutex->value.unlock(); }
enum OSEventMode { OS_EVENT_MODE_MANUAL, OS_EVENT_MODE_AUTO };
struct OSEvent { std::mutex mutex; std::condition_variable condition; bool signaled{}, automatic{}; };
inline void OSInitEvent(OSEvent* event, BOOL signaled, OSEventMode mode) {
    event->signaled = signaled != 0; event->automatic = mode == OS_EVENT_MODE_AUTO;
}
inline void OSSignalEvent(OSEvent* event) {
    std::lock_guard lock(event->mutex); event->signaled = true; event->condition.notify_all();
}
inline BOOL OSWaitEventWithTimeout(OSEvent* event, OSTime timeout) {
    std::unique_lock lock(event->mutex);
    const bool result = event->condition.wait_for(lock, std::chrono::nanoseconds(timeout), [&] { return event->signaled; });
    if (result && event->automatic) event->signaled = false;
    return result ? TRUE : FALSE;
}
using OSThreadEntryPointFn = int (*)(int, const char**);
using OSThreadAttributes = std::uint32_t;
enum { OS_THREAD_ATTRIB_AFFINITY_CPU0 = 1, OS_THREAD_ATTRIB_AFFINITY_CPU1 = 2,
    OS_THREAD_ATTRIB_AFFINITY_CPU2 = 4, OS_THREAD_ATTRIB_AFFINITY_ANY = 7 };
struct OSThread {
    std::thread native;
    OSThreadEntryPointFn entry{};
    int argc{};
    std::uint32_t affinity{};
    const char** argv{};
};
namespace svc_test_platform {
inline std::atomic<unsigned> created{}, resumed{}, exited{};
inline std::int32_t priority{};
inline bool stack_aligned{}, fail_create{};
inline std::uint32_t affinity{};
inline bool fail_affinity{};
inline unsigned affinity_calls{};
struct Exit {};
}
inline BOOL OSCreateThread(OSThread* thread, OSThreadEntryPointFn entry, std::int32_t argc,
                           char* argv, void* stack, std::uint32_t size, std::int32_t priority,
                           OSThreadAttributes affinity) {
    svc_test_platform::stack_aligned = reinterpret_cast<std::uintptr_t>(stack) % 16 == 0 && size % 16 == 0;
    svc_test_platform::priority = priority;
    if (svc_test_platform::fail_create) return FALSE;
    thread->affinity = affinity; svc_test_platform::affinity = affinity;
    thread->entry = entry; thread->argc = argc; thread->argv = reinterpret_cast<const char**>(argv);
    ++svc_test_platform::created; return TRUE;
}
inline BOOL OSSetThreadAffinity(OSThread* thread, std::uint32_t affinity) {
    ++svc_test_platform::affinity_calls;
    if (svc_test_platform::fail_affinity) return FALSE;
    thread->affinity = affinity; svc_test_platform::affinity = affinity; return TRUE;
}
inline std::int32_t OSResumeThread(OSThread* thread) {
    ++svc_test_platform::resumed;
    thread->native = std::thread([thread] {
        try { (void)thread->entry(thread->argc, thread->argv); }
        catch (const svc_test_platform::Exit&) {}
    });
    return 1;
}
inline void OSExitThread(std::int32_t) { ++svc_test_platform::exited; throw svc_test_platform::Exit{}; }
inline BOOL OSJoinThread(OSThread* thread, int*) {
    if (thread->native.joinable()) thread->native.join();
    return TRUE;
}
inline void OSSleepTicks(OSTime ticks) { std::this_thread::sleep_for(std::chrono::microseconds(ticks)); }
