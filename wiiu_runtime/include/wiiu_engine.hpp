#pragma once

#include "arm11_types.hpp"
#include "wiiu_memory.hpp"
#include "hid_manager.hpp"
#include "svc_handler.hpp"
#include "audio_driver.hpp"
#include <coreinit/mutex.h>

#include <cstdint>
#include <unordered_map>

namespace wiiu {

class WiiUEngine {
public:
    explicit WiiUEngine(bool is_new_3ds = false);
    ~WiiUEngine();
    using BlockFunction = void (*)(arm11::CPUContext&, WiiUMemory&);
    // Returns the name of the module (e.g. a CRO) containing address, or nullptr.
    using ModuleLookup = const char* (*)(void* context, std::uint32_t address);

    // Low-bit-tagged addresses select Thumb; canonical even keys select ARM.
    // Replaces an existing entry; null callbacks throw std::invalid_argument.
    void RegisterBlock(std::uint32_t address, BlockFunction block_func);

    [[nodiscard]] arm11::CPUContext& GetCPU() noexcept { return cpu; }
    [[nodiscard]] WiiUMemory& GetMemory() noexcept { return memory; }
    [[nodiscard]] AudioDriver& GetAudioDriver() noexcept { return audio_; }
    [[nodiscard]] HIDManager& GetHIDManager() noexcept { return hid_manager; }
    [[nodiscard]] bool IsHalted() const noexcept { return halted_; }

    // Opt-in (default off): a missing block also logs one bounded "Missing block detail" line.
    void EnableMissingBlockDiagnostics(bool enabled) noexcept { missing_block_diagnostics_ = enabled; }
    // Only used by missing-block diagnostics; the callback must not throw.
    void SetModuleLookup(ModuleLookup lookup, void* context) noexcept {
        module_lookup_ = lookup;
        module_lookup_context_ = context;
    }

    // Stop policy: a block runs only for its exact lookup key (address | Thumb bit).
    // A miss logs PC, mode and key, never calls host code at the guest PC, and halts the
    // main CPU (IsHalted() true, Run() returns). Worker contexts return false without
    // halting; the SVC layer decides. This is a fail-closed stop, not an interpreter fallback.
    //
    // Each block must assign the next guest PC to cpu.r[15] before returning.
    // An odd entry selects Thumb; a canonical even entry preserves current mode.
    // Yields after a bounded block slice so input, ProcUI, and rendering can run.
    // This is cooperative scheduling, not guest VBlank or OS-trap emulation.
    void Run(std::uint32_t entry_point);

private:
    static bool RunWorkerBlock(void* engine, arm11::CPUContext& context);
    bool DispatchOne(arm11::CPUContext& context);
    void LogMissingBlockDetail(const arm11::CPUContext& context) const;
    arm11::CPUContext cpu{};
    HIDManager hid_manager{};
    WiiUMemory memory{};
    AudioDriver audio_{memory};
    SvcHandler svc_handler_;
    OSMutex execution_mutex_{};
    std::unordered_map<std::uint32_t, BlockFunction> block_table;
    bool halted_{};
    bool execution_started_{};
    bool missing_block_diagnostics_{};
    ModuleLookup module_lookup_{};
    void* module_lookup_context_{};
    // Key of the last block run on the main CPU; has_last_block_ is false until one runs.
    bool has_last_block_{};
    std::uint32_t last_block_key_{};
};

} // namespace wiiu
