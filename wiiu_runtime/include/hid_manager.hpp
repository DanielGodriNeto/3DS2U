#pragma once

#include <cstdint>

namespace wiiu {

// MVP HID register bridge; real 3DS HID shared-memory/service emulation is separate.
class HIDManager {
public:
    static constexpr std::uint32_t kBase = 0x1EC00000u;
    static constexpr std::uint32_t kEnd = kBase + 0x24u;

    void Update();
    // Numeric LE guest value; callers must not byte-swap this value again.
    [[nodiscard]] std::uint32_t ReadRegister(std::uint32_t address,
                                            unsigned width = 4) const;

private:
    // +00 held, +04 pressed, +08 released, +0C x, +10 y, +14 touched.
    // +18, +1C and +20 are reserved and read as zero.
    std::uint32_t held_{}, down_{}, up_{}, x_{}, y_{}, touched_{};
};

} // namespace wiiu
