#pragma once

#include <cstdint>

namespace wiiu {
namespace gx2 {
class GX2Manager;
}

// MVP register bridge; complete PICA200 command decoding remains separate.
class Pica200 {
public:
    static constexpr std::uint32_t kMMIOBase = 0x1EF00000u;
    static constexpr std::uint32_t kMMIOEnd = 0x1EFFFFFFu; // Exclusive intercept limit.
    static constexpr std::uint32_t kClearColorRegister = kMMIOBase;

    // The graphics manager must outlive this non-owning bridge.
    explicit Pica200(gx2::GX2Manager& manager) noexcept : gx2_manager_(manager) {}

    // Values are guest register integers, before any FCRAM byte swapping.
    void WriteMMIO(std::uint32_t address, std::uint32_t value);

private:
    gx2::GX2Manager& gx2_manager_;
};

} // namespace wiiu
