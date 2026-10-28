#pragma once

#include "arm11_types.hpp"
#include "wiiu_memory.hpp"
#include "hid_manager.hpp"

#include <cstdint>
#include <unordered_map>

namespace wiiu {

class WiiUEngine {
public:
    WiiUEngine();
    using BlockFunction = void (*)(arm11::CPUContext&, WiiUMemory&);

    // Low-bit-tagged addresses select Thumb; canonical even keys select ARM.
    // Replaces an existing entry; null callbacks throw std::invalid_argument.
    void RegisterBlock(std::uint32_t address, BlockFunction block_func);

    [[nodiscard]] arm11::CPUContext& GetCPU() noexcept { return cpu; }
    [[nodiscard]] WiiUMemory& GetMemory() noexcept { return memory; }
    [[nodiscard]] bool IsHalted() const noexcept { return halted_; }

    // Each block must assign the next guest PC to cpu.r[15] before returning.
    // An odd entry selects Thumb; a canonical even entry preserves current mode.
    // Yields after a bounded block slice so input, ProcUI, and rendering can run.
    // This is cooperative scheduling, not guest VBlank or OS-trap emulation.
    void Run(std::uint32_t entry_point);

private:
    arm11::CPUContext cpu{};
    HIDManager hid_manager{};
    WiiUMemory memory{};
    std::unordered_map<std::uint32_t, BlockFunction> block_table;
    bool halted_{};
};

} // namespace wiiu
