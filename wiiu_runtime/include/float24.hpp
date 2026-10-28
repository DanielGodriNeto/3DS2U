#pragma once

#include <bit>
#include <cstdint>
#include <limits>

namespace wiiu {

// Numeric PICA s1/e7/m16 value, exponent bias 63; independent of host byte order.
// Preserves subnormals losslessly; emulating PICA arithmetic flush is separate.
[[nodiscard]] inline constexpr float float24_to_float32(std::uint32_t f24) noexcept {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    const auto sign = (f24 & 0x800000u) << 8;
    const auto exponent = (f24 >> 16) & 0x7Fu;
    const auto mantissa = f24 & 0xFFFFu;
    std::uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) bits = sign;
        else {
            const auto shift = static_cast<unsigned>(std::countl_zero(mantissa) - 15);
            bits = sign | ((65u - shift) << 23) | (((mantissa << shift) & 0xFFFFu) << 7);
        }
    } else if (exponent == 0x7Fu) {
        bits = sign | 0x7F800000u | (mantissa << 7);
    } else {
        bits = sign | ((exponent + 64u) << 23) | (mantissa << 7);
    }
    return std::bit_cast<float>(bits);
}

} // namespace wiiu
