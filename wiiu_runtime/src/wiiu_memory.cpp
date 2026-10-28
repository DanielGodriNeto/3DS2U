#include "wiiu_memory.hpp"
#include "pica200.hpp"
#include "hid_manager.hpp"
#include "hle_services.hpp"

#include <coreinit/memdefaultheap.h>

#include <bit>
#include <cstring>
#include <new>
#include <stdexcept>
#include <algorithm>
#if defined(_MSC_VER)
#include <cstdlib>
#endif

namespace wiiu {

static_assert(std::endian::native == std::endian::big ||
              std::endian::native == std::endian::little);
static_assert(WiiUMemory::kFCRAMSize % WiiUMemory::kPageSize == 0);

namespace {
// PPC swaps every FCRAM access; LE hosts can exercise the same memory paths.
std::uint32_t GuestEndian(std::uint32_t value) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return value;
    } else {
#if defined(_MSC_VER)
        return _byteswap_ulong(value);
#else
        return __builtin_bswap32(value);
#endif
    }
}
std::uint16_t GuestEndian(std::uint16_t value) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return value;
    } else {
#if defined(_MSC_VER)
        return _byteswap_ushort(value);
#else
        return __builtin_bswap16(value);
#endif
    }
}
} // namespace

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

void WiiUMemory::SetPica200(Pica200& pica200) noexcept {
    pica200_ = &pica200;
    pica200.AttachMemory(*this);
}

const void* WiiUMemory::GetPointer(std::uint32_t addr, std::size_t size) const {
    return fcram_ + Offset(addr, size);
}

void WiiUMemory::LoadImage(std::uint32_t addr, std::span<const std::uint8_t> bytes) {
    const auto offset = Offset(addr, bytes.size());
    if (!bytes.empty()) std::memcpy(fcram_ + offset, bytes.data(), bytes.size());
}

void WiiUMemory::ClearRange(std::uint32_t addr, std::size_t size) {
    const auto offset = Offset(addr, size);
    if (size != 0) std::memset(fcram_ + offset, 0, size);
}

void WiiUMemory::ReserveImageRange(std::uint32_t address, std::size_t size) {
    const auto offset = Offset(address, size);
    image_end_ = std::max(image_end_, kVirtualBase + static_cast<std::uint32_t>(offset + size));
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
    if (addr >= hle::HLEServiceManager::kAudioMMIOBase && addr < hle::HLEServiceManager::kAudioMMIOEnd)
        return hle::HLEServiceManager::ReadAudioMMIO(addr, 4);
    if (addr >= HIDManager::kBase && addr < HIDManager::kEnd) {
        if (hid_ == nullptr) throw std::logic_error("HID MMIO bridge is not attached");
        return hid_->ReadRegister(addr);
    }
    std::uint32_t value;
    // memcpy avoids unaligned PPC loads and aliasing violations. ARM-specific
    // alignment/rotation semantics remain the instruction translator's job.
    std::memcpy(&value, fcram_ + Offset(addr, sizeof(value)), sizeof(value));
    return GuestEndian(value);
}

void WiiUMemory::Write32(std::uint32_t addr, std::uint32_t val) {
    if (addr >= hle::HLEServiceManager::kAudioMMIOBase && addr < hle::HLEServiceManager::kAudioMMIOEnd) {
        hle::HLEServiceManager::WriteAudioMMIO(addr, val, 4);
        return;
    }
    if (addr >= Pica200::kMMIOBase && addr < Pica200::kMMIOEnd) {
        if (!gpu_access_allowed_) throw std::logic_error("GX2 MMIO is restricted to the main guest thread");
        if (pica200_ == nullptr) {
            throw std::logic_error("PICA200 MMIO bridge is not attached");
        }
        // The MMIO bridge consumes numeric register values, not swapped bytes.
        pica200_->WriteMMIO(addr, val);
        if (addr == Pica200::kDrawRegister && val == 1) frame_requested_ = true;
        return;
    }
    const auto offset = Offset(addr, sizeof(val));
    const auto little_endian = GuestEndian(val);
    std::memcpy(fcram_ + offset, &little_endian, sizeof(little_endian));
}

std::uint16_t WiiUMemory::Read16(std::uint32_t addr) const {
    if (addr >= hle::HLEServiceManager::kAudioMMIOBase && addr < hle::HLEServiceManager::kAudioMMIOEnd)
        return static_cast<std::uint16_t>(hle::HLEServiceManager::ReadAudioMMIO(addr, 2));
    if (addr >= HIDManager::kBase && addr < HIDManager::kEnd) {
        if (hid_ == nullptr) throw std::logic_error("HID MMIO bridge is not attached");
        return static_cast<std::uint16_t>(hid_->ReadRegister(addr, 2));
    }
    std::uint16_t value;
    std::memcpy(&value, fcram_ + Offset(addr, sizeof(value)), sizeof(value));
    return GuestEndian(value);
}

void WiiUMemory::Write16(std::uint32_t addr, std::uint16_t val) {
    if (addr >= hle::HLEServiceManager::kAudioMMIOBase && addr < hle::HLEServiceManager::kAudioMMIOEnd) {
        hle::HLEServiceManager::WriteAudioMMIO(addr, val, 2);
        return;
    }
    const auto offset = Offset(addr, sizeof(val));
    const auto little_endian = GuestEndian(val);
    std::memcpy(fcram_ + offset, &little_endian, sizeof(little_endian));
}

} // namespace wiiu
