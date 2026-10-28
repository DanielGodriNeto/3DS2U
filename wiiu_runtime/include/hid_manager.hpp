#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace wiiu {

// VPAD bridge with HID/IRRST ring snapshots and explicit runtime address aliases.
class HIDManager {
  public:
    static constexpr std::uint32_t kBase = 0x1EC00000u;
    static constexpr std::uint32_t kEnd = kBase + 0x24u;
    static constexpr std::uint32_t kSharedBase = 0x1EC01000u;
    static constexpr std::uint32_t kSharedSize = 0x2B0u;
    static constexpr std::uint32_t kIRRSTBase = 0x1EC02000u;
    static constexpr std::uint32_t kIRRSTSize = 0x98u;
    explicit HIDManager(bool is_new_3ds = false) : is_new_3ds_(is_new_3ds) {}
    [[nodiscard]] static constexpr bool IsMappedAddress(std::uint32_t address) noexcept {
        return (address >= kBase && address < kEnd) ||
               (address >= kSharedBase && address < kSharedBase + kSharedSize) ||
               (address >= kIRRSTBase && address < kIRRSTBase + kIRRSTSize);
    }
    // Raw guest-LE snapshots may also be copied into a service-selected guest mapping.
    [[nodiscard]] std::span<const std::uint8_t> GetSharedMemory() const noexcept { return shared_; }
    [[nodiscard]] std::span<const std::uint8_t> GetIRRSTSharedMemory() const noexcept { return irrst_; }

    void Update();
    // Numeric LE guest value; callers must not byte-swap this value again.
    [[nodiscard]] std::uint32_t ReadRegister(std::uint32_t address, unsigned width = 4) const;

  private:
    // +00 held, +04 pressed, +08 released, +0C x, +10 y, +14 touched.
    // +18 circle axes, +1C C-Stick axes (packed s16 x/y), +20 extended held keys.
    std::uint32_t held_{}, down_{}, up_{}, x_{}, y_{}, touched_{};
    std::int16_t circle_x_{}, circle_y_{}, cstick_x_{}, cstick_y_{};
    bool is_new_3ds_{};
    std::uint64_t sequence_{};
    std::array<std::uint8_t, kSharedSize> shared_{};
    std::array<std::uint8_t, kIRRSTSize> irrst_{};
    void Publish(std::uint32_t previous_held);
};

} // namespace wiiu
