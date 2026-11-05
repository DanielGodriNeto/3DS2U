#pragma once

// Host stand-in for wiiu_runtime/include/wiiu_engine.hpp. It keeps the call surface that
// translated output uses (no wut, threads or mutexes). Guest memory is a sparse byte store.

#include "arm11_types.hpp"
#include "svc_handler.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <unordered_map>

namespace wiiu {

class WiiUMemory {
public:
    // The sparse store already covers every guest address, so these calls only mark intent.
    void MapImageRange(std::uint32_t, std::uint32_t, std::uint32_t) {}
    void ReserveImageRange(std::uint32_t, std::size_t) {}

    void LoadImage(std::uint32_t addr, std::span<const std::uint8_t> bytes) {
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            Write8(addr + static_cast<std::uint32_t>(i), bytes[i]);
        }
    }

    void ClearRange(std::uint32_t addr, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) bytes_.erase(addr + static_cast<std::uint32_t>(i));
    }

    [[nodiscard]] std::uint8_t Read8(std::uint32_t addr) const {
        const auto found = bytes_.find(addr);
        return found == bytes_.end() ? std::uint8_t{0} : found->second;
    }

    void Write8(std::uint32_t addr, std::uint8_t value) { bytes_[addr] = value; }

    [[nodiscard]] std::uint16_t Read16(std::uint32_t addr) const {
        return static_cast<std::uint16_t>(Read8(addr) | (Read8(addr + 1) << 8));
    }

    void Write16(std::uint32_t addr, std::uint16_t value) {
        Write8(addr, static_cast<std::uint8_t>(value));
        Write8(addr + 1, static_cast<std::uint8_t>(value >> 8));
    }

    [[nodiscard]] std::uint32_t Read32(std::uint32_t addr) const {
        return static_cast<std::uint32_t>(Read16(addr)) |
               (static_cast<std::uint32_t>(Read16(addr + 2)) << 16);
    }

    void Write32(std::uint32_t addr, std::uint32_t value) {
        Write16(addr, static_cast<std::uint16_t>(value));
        Write16(addr + 2, static_cast<std::uint16_t>(value >> 16));
    }

private:
    std::unordered_map<std::uint32_t, std::uint8_t> bytes_;
};

class WiiUEngine {
public:
    using BlockFunction = void (*)(arm11::CPUContext&, WiiUMemory&);

    void RegisterBlock(std::uint32_t address, BlockFunction block_func) {
        if (block_func == nullptr) throw std::invalid_argument("null block function");
        blocks_[address] = block_func;
    }

    [[nodiscard]] arm11::CPUContext& GetCPU() noexcept { return cpu_; }
    [[nodiscard]] WiiUMemory& GetMemory() noexcept { return memory_; }

    // Returns the block registered under a DispatchKey-style key, or nullptr.
    [[nodiscard]] BlockFunction FindBlock(std::uint32_t key) const {
        const auto found = blocks_.find(key);
        return found == blocks_.end() ? nullptr : found->second;
    }

private:
    arm11::CPUContext cpu_{};
    WiiUMemory memory_{};
    std::unordered_map<std::uint32_t, BlockFunction> blocks_;
};

} // namespace wiiu
