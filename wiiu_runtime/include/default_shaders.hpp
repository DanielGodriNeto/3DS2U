#pragma once
#include <array>
#include <cstdint>

namespace wiiu::gx2::shaders {

// Minimal Latte CF programs, serialized LE before GPU use. No SDK shader binary
// or runtime GLSL compiler is required. R1=position, R2=color, R3=clip position.
// Four DOT4 groups compute R3 = row-major mat4(c0..c3) * R1.
inline constexpr auto vertex = [] {
    std::array<std::uint32_t, 40> words{};
    words[0] = 0; words[1] = 0x09800000u; // CALL_FS, no barrier
    words[2] = 4; words[3] = 0xA03C0000u; // ALU: address 4, 16 instructions, barrier
    words[4] = 0xC001A03Cu; words[5] = 0x94000688u; // POS0 = R3
    words[6] = 0xC0014000u; words[7] = 0x14200688u; // PARAM0 = R2, end program
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned lane = 0; lane < 4; ++lane) {
            const auto offset = 8u + (row * 4u + lane) * 2u;
            words[offset] = 1u | (lane << 10) | ((256u + row) << 13) |
                            (lane << 23) | (lane == 3 ? 0x80000000u : 0u);
            // Reduction uses all four lanes; only the row's destination lane writes.
            words[offset + 1] = (0x50u << 7) | (3u << 21) | (lane << 29) |
                                (lane == row ? 0x10u : 0u);
        }
    }
    return words;
}();
inline constexpr std::array<std::uint32_t, 2> pixel{
    0xC0000000u, 0x94200688u, // EXP_DONE PIX0, R0.xyzw, end program
};

} // namespace wiiu::gx2::shaders
