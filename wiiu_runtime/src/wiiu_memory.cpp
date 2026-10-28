#include "wiiu_memory.hpp"
#include "pica200.hpp"

#include <coreinit/memdefaultheap.h>

#include <bit>
#include <cstring>
#include <new>
#include <stdexcept>

namespace wiiu {

// Unconditional swapping is correct only for this big-endian runtime target.
static_assert(std::endian::native == std::endian::big,
              "WiiUMemory requires a big-endian PowerPC target");
static_assert(WiiUMemory::kFCRAMSize % WiiUMemory::kPageSize == 0);

WiiUMemory::WiiUMemory()
    : fcram_(static_cast<std::byte*>(MEMAllocFromDefaultHeapEx(
          kFCRAMSize, static_cast<std::int32_t>(kPageSize)))) {
    if (fcram_ == nullptr) {
        throw std::bad_alloc{};
    }
    std::memset(fcram_, 0, kFCRAMSize);
}

WiiUMemory::~WiiUMemory() noexcept {
    MEMFreeToDefaultHeap(fcram_);
}

std::size_t WiiUMemory::Offset(std::uint32_t addr, std::size_t width) {
    if (addr < kVirtualBase) {
        throw std::out_of_range("3DS address below FCRAM virtual window");
    }
    const auto offset = static_cast<std::size_t>(addr - kVirtualBase);
    // Check by subtraction so a wrapping guest address cannot pass the bounds.
    if (offset > kFCRAMSize || width > kFCRAMSize - offset) {
        throw std::out_of_range("3DS access outside FCRAM virtual window");
    }
    return offset;
}

std::uint32_t WiiUMemory::Read32(std::uint32_t addr) const {
    std::uint32_t value;
    // memcpy avoids unaligned PPC loads and aliasing violations. ARM-specific
    // alignment/rotation semantics remain the instruction translator's job.
    std::memcpy(&value, fcram_ + Offset(addr, sizeof(value)), sizeof(value));
    return __builtin_bswap32(value);
}

void WiiUMemory::Write32(std::uint32_t addr, std::uint32_t val) {
    if (addr >= Pica200::kMMIOBase && addr < Pica200::kMMIOEnd) {
        if (pica200_ == nullptr) {
            throw std::logic_error("PICA200 MMIO bridge is not attached");
        }
        // The MMIO bridge consumes numeric register values, not swapped bytes.
        pica200_->WriteMMIO(addr, val);
        return;
    }
    const auto offset = Offset(addr, sizeof(val));
    const auto little_endian = __builtin_bswap32(val);
    std::memcpy(fcram_ + offset, &little_endian, sizeof(little_endian));
}

std::uint16_t WiiUMemory::Read16(std::uint32_t addr) const {
    std::uint16_t value;
    std::memcpy(&value, fcram_ + Offset(addr, sizeof(value)), sizeof(value));
    return __builtin_bswap16(value);
}

void WiiUMemory::Write16(std::uint32_t addr, std::uint16_t val) {
    const auto offset = Offset(addr, sizeof(val));
    const auto little_endian = __builtin_bswap16(val);
    std::memcpy(fcram_ + offset, &little_endian, sizeof(little_endian));
}

} // namespace wiiu
