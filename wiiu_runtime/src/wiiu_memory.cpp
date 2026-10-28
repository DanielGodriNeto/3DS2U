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
static_assert(WiiUMemory::kNewFCRAMSize % WiiUMemory::kPageSize == 0);

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

WiiUMemory::WiiUMemory(bool is_new_3ds)
    : fcram_size_(is_new_3ds ? kNewFCRAMSize : kFCRAMSize),
      fcram_(static_cast<std::byte*>(MEMAllocFromDefaultHeapEx(
          fcram_size_, static_cast<std::int32_t>(kPageSize)))) {
    if (fcram_ == nullptr) {
        throw std::bad_alloc{};
    }
    std::memset(fcram_, 0, fcram_size_);
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
    if (!dynamic_ranges_.empty()) throw std::logic_error("Reserve images before dynamic allocations");
    image_end_ = std::max(image_end_, kVirtualBase + static_cast<std::uint32_t>(offset + size));
}

std::uint32_t WiiUMemory::FindFreeRange(std::uint32_t size) const {
    if (size == 0 || (size & (kPageSize - 1u)) != 0) return 0;
    auto address = (image_end_ + kPageSize - 1u) & ~(kPageSize - 1u);
    for (const auto& [start, length] : dynamic_ranges_) {
        if (size <= start - address) return address;
        address = start + length;
    }
    return size <= GetVirtualEnd() - address ? address : 0;
}

bool WiiUMemory::ReserveDynamicRange(std::uint32_t address, std::uint32_t size) {
    const auto end = GetVirtualEnd();
    if (size == 0 || ((address | size) & (kPageSize - 1u)) != 0 ||
        address < image_end_ || address > end || size > end - address) return false;
    for (const auto& [start, length] : dynamic_ranges_)
        if (address < start + length && start < address + size) return false;
    return dynamic_ranges_.emplace(address, size).second;
}

bool WiiUMemory::ReleaseDynamicRange(std::uint32_t address, std::uint32_t size) noexcept {
    const auto it = dynamic_ranges_.find(address);
    if (it == dynamic_ranges_.end() || it->second != size) return false;
    dynamic_ranges_.erase(it);
    return true;
}

std::size_t WiiUMemory::Offset(std::uint32_t addr, std::size_t width) const {
    if (addr < kVirtualBase) {
        throw std::out_of_range("3DS address below FCRAM virtual window");
    }
    const auto offset = static_cast<std::size_t>(addr - kVirtualBase);
    // Check by subtraction so a wrapping guest address cannot pass the bounds.
    if (offset > fcram_size_ || width > fcram_size_ - offset) {
        throw std::out_of_range("3DS access outside FCRAM virtual window");
    }
    return offset;
}

std::uint32_t WiiUMemory::Read32(std::uint32_t addr) const {
    if (addr >= hle::HLEServiceManager::kAudioMMIOBase && addr < hle::HLEServiceManager::kAudioMMIOEnd)
        return hle::HLEServiceManager::ReadAudioMMIO(addr, 4, audio_);
    if (HIDManager::IsMappedAddress(addr)) {
        if (hid_ == nullptr) throw std::logic_error("HID MMIO bridge is not attached");
        return hid_->ReadRegister(addr);
    }
    std::uint32_t value;
    // memcpy avoids unaligned PPC loads and aliasing violations. ARM-specific
    // alignment/rotation semantics remain the instruction translator's job.
    std::memcpy(&value, fcram_ + Offset(addr, sizeof(value)), sizeof(value));
    return GuestEndian(value);
}

std::uint8_t WiiUMemory::Read8(std::uint32_t addr) const {
    if (HIDManager::IsMappedAddress(addr)) {
        if (hid_ == nullptr) throw std::logic_error("HID bridge is not attached");
        return static_cast<std::uint8_t>(hid_->ReadRegister(addr, 1));
    }
    return std::to_integer<std::uint8_t>(fcram_[Offset(addr, 1)]);
}
void WiiUMemory::Write8(std::uint32_t addr, std::uint8_t value) {
    fcram_[Offset(addr, 1)] = static_cast<std::byte>(value);
}

void WiiUMemory::Write32(std::uint32_t addr, std::uint32_t val) {
    if (addr >= hle::HLEServiceManager::kAudioMMIOBase && addr < hle::HLEServiceManager::kAudioMMIOEnd) {
        hle::HLEServiceManager::WriteAudioMMIO(addr, val, 4, audio_);
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
        return static_cast<std::uint16_t>(hle::HLEServiceManager::ReadAudioMMIO(addr, 2, audio_));
    if (HIDManager::IsMappedAddress(addr)) {
        if (hid_ == nullptr) throw std::logic_error("HID MMIO bridge is not attached");
        return static_cast<std::uint16_t>(hid_->ReadRegister(addr, 2));
    }
    std::uint16_t value;
    std::memcpy(&value, fcram_ + Offset(addr, sizeof(value)), sizeof(value));
    return GuestEndian(value);
}

void WiiUMemory::Write16(std::uint32_t addr, std::uint16_t val) {
    if (addr >= hle::HLEServiceManager::kAudioMMIOBase && addr < hle::HLEServiceManager::kAudioMMIOEnd) {
        hle::HLEServiceManager::WriteAudioMMIO(addr, val, 2, audio_);
        return;
    }
    const auto offset = Offset(addr, sizeof(val));
    const auto little_endian = GuestEndian(val);
    std::memcpy(fcram_ + offset, &little_endian, sizeof(little_endian));
}

} // namespace wiiu
