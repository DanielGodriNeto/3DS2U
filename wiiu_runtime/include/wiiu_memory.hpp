#pragma once

#include <cstddef>
#include <cstdint>

namespace wiiu {

// Owns a page-aligned host buffer for the guest window [0x08000000, 0x10000000).
// This is a software address mapping; it does not map guest addresses into PPC VA.
class WiiUMemory {
public:
    static constexpr std::uint32_t kVirtualBase = 0x08000000u;
    static constexpr std::uint32_t kFCRAMSize = 128u * 1024u * 1024u;
    static constexpr std::uint32_t kPageSize = 0x1000u;

    // Requires initialized wut default heaps; throws std::bad_alloc on failure.
    WiiUMemory();
    ~WiiUMemory() noexcept;

    WiiUMemory(const WiiUMemory&) = delete;
    WiiUMemory& operator=(const WiiUMemory&) = delete;
    WiiUMemory(WiiUMemory&&) = delete;
    WiiUMemory& operator=(WiiUMemory&&) = delete;

    // All word/halfword accesses swap guest LE bytes to/from native PPC values.
    // Out-of-window accesses throw std::out_of_range before touching host memory.
    [[nodiscard]] std::uint32_t Read32(std::uint32_t addr) const;
    void Write32(std::uint32_t addr, std::uint32_t val);
    [[nodiscard]] std::uint16_t Read16(std::uint32_t addr) const;
    void Write16(std::uint32_t addr, std::uint16_t val);

private:
    [[nodiscard]] static std::size_t Offset(std::uint32_t addr, std::size_t width);

    std::byte* fcram_;
};

} // namespace wiiu
