#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <map>

namespace wiiu {

class Pica200;
class HIDManager;
class AudioDriver;

// Owns 128 MiB (O3DS) or 256 MiB (N3DS), starting at guest VA 0x08000000.
// This is a software address mapping; it does not map guest addresses into PPC VA.
class WiiUMemory {
public:
    static constexpr std::uint32_t kVirtualBase = 0x08000000u;
    // Legacy size constant; use GetSize() for all instance bounds checks.
    static constexpr std::uint32_t kFCRAMSize = 128u * 1024u * 1024u;
    static constexpr std::uint32_t kNewFCRAMSize = 256u * 1024u * 1024u;
    static constexpr std::uint32_t kPageSize = 0x1000u;

    // Requires initialized wut default heaps; throws std::bad_alloc on failure.
    explicit WiiUMemory(bool is_new_3ds = false);
    ~WiiUMemory() noexcept;

    WiiUMemory(const WiiUMemory&) = delete;
    WiiUMemory& operator=(const WiiUMemory&) = delete;
    WiiUMemory(WiiUMemory&&) = delete;
    WiiUMemory& operator=(WiiUMemory&&) = delete;
    [[nodiscard]] std::uint32_t GetSize() const noexcept { return fcram_size_; }
    [[nodiscard]] std::uint32_t GetVirtualEnd() const noexcept { return kVirtualBase + fcram_size_; }
    [[nodiscard]] bool IsNew3DS() const noexcept { return fcram_size_ == kNewFCRAMSize; }

    // The attached bridge must outlive all MMIO writes through this memory.
    void SetPica200(Pica200& pica200) noexcept;
    // The input owner must outlive guest reads through this memory.
    void SetHIDManager(HIDManager& hid) noexcept { hid_ = &hid; }
    // Detach before destroying the audio owner; null preserves silent stub behavior.
    void SetAudioDriver(AudioDriver* audio) noexcept { audio_ = audio; }
    [[nodiscard]] const void* GetPointer(std::uint32_t addr, std::size_t size = 1) const;
    // Maps a page-aligned guest image range to a page-aligned FCRAM backing range.
    // Throws std::out_of_range for zero size, misalignment, 32-bit guest wrap or
    // backing outside FCRAM, and std::logic_error for overlapping virtual ranges,
    // shared backing, dynamic-range overlap or a non-identity range inside the
    // canonical window. A failed call leaves the mapper unchanged. See
    // docs/guest-memory-mapping.md.
    void MapImageRange(std::uint32_t guest_va, std::uint32_t image_offset, std::uint32_t size);
    void LoadImage(std::uint32_t addr, std::span<const std::uint8_t> bytes);
    void ClearRange(std::uint32_t addr, std::size_t size);
    // A draw command yields the cooperative CPU slice to presentation.
    [[nodiscard]] bool ConsumeFrameRequest() noexcept {
        const bool requested = frame_requested_;
        frame_requested_ = false;
        return requested;
    }
    void RequestFrame() noexcept { frame_requested_ = true; }
    // Register image/BSS ranges before execution so the heap cannot overwrite them.
    void ReserveImageRange(std::uint32_t address, std::size_t size);
    [[nodiscard]] std::uint32_t GetImageEnd() const noexcept { return image_end_; }
    // Call under the engine execution lock, shared by CRO loading and SVC heaps.
    [[nodiscard]] std::uint32_t FindFreeRange(std::uint32_t size) const;
    bool ReserveDynamicRange(std::uint32_t address, std::uint32_t size);
    bool ReleaseDynamicRange(std::uint32_t address, std::uint32_t size) noexcept;
    void SetGPUAccessAllowed(bool allowed) noexcept { gpu_access_allowed_ = allowed; }

    // FCRAM word/halfword accesses swap guest LE bytes to/from native PPC values.
    // HID/audio reads and audio/PICA200 writes use numeric MMIO values directly.
    // Other out-of-window accesses throw std::out_of_range.
    [[nodiscard]] std::uint32_t Read32(std::uint32_t addr) const;
    [[nodiscard]] std::uint8_t Read8(std::uint32_t addr) const;
    // True when every byte of [addr, addr + size) is backed; never throws.
    [[nodiscard]] bool IsMapped(std::uint32_t addr, std::uint32_t size = 1) const noexcept;
    void Write8(std::uint32_t addr, std::uint8_t value);
    void Write32(std::uint32_t addr, std::uint32_t val);
    [[nodiscard]] std::uint16_t Read16(std::uint32_t addr) const;
    void Write16(std::uint32_t addr, std::uint16_t val);

private:
    [[nodiscard]] std::size_t Offset(std::uint32_t addr, std::size_t width) const;
    void ValidateImageMapping(std::uint32_t guest_va, std::uint32_t image_offset,
                              std::uint32_t size) const;

    struct MappedRange {
        std::uint32_t image_offset;
        std::uint32_t size;
    };

    std::uint32_t fcram_size_;
    std::byte* fcram_;
    Pica200* pica200_{};
    HIDManager* hid_{};
    AudioDriver* audio_{};
    bool frame_requested_{};
    std::uint32_t image_end_ = kVirtualBase + kPageSize;
    bool gpu_access_allowed_ = true;
    // Guest VA -> backing offset and mapped size. Image sections may use VAs
    // below kVirtualBase while sharing the same FCRAM allocation.
    std::map<std::uint32_t, MappedRange> image_ranges_;
    // Backing offset -> size; keeps two image VAs from sharing FCRAM bytes.
    std::map<std::uint32_t, std::uint32_t> image_backing_;
    std::map<std::uint32_t, std::uint32_t> dynamic_ranges_;
};

} // namespace wiiu
