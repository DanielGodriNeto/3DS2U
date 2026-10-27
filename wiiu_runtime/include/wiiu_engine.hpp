#pragma once

#include "arm11_types.hpp"
#include "wiiu_memory.hpp"

#include <cstdint>
#include <unordered_map>

namespace wiiu {

class WiiUEngine {
public:
    using BlockFunction = void (*)(arm11::CPUContext&, WiiUMemory&);

    // Replaces an existing entry; null callbacks throw std::invalid_argument.
    void RegisterBlock(std::uint32_t address, BlockFunction block_func);

    // Each block must assign the next guest PC to cpu.r[15] before returning.
    // Repeated PCs are valid guest loops; an unmapped PC ends this run.
    void Run(std::uint32_t entry_point);

private:
    arm11::CPUContext cpu{};
    WiiUMemory memory{};
    std::unordered_map<std::uint32_t, BlockFunction> block_table;
};

} // namespace wiiu
