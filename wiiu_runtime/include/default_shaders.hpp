#pragma once
#include <array>
#include <cstdint>

namespace wiiu::gx2::shaders {

// Minimal Latte CF programs, serialized LE before GPU use. No SDK shader binary
// or runtime GLSL compiler is required. R1=position, R2=color, PARAM0=PS input R0.
inline constexpr std::array<std::uint32_t, 6> vertex{
    0x00000000u, 0x09800000u, // CALL_FS, no barrier
    0xC000A03Cu, 0x94000688u, // EXP_DONE POS0, R1.xyzw
    0xC0014000u, 0x14200688u, // EXP_DONE PARAM0, R2.xyzw, end program
};
inline constexpr std::array<std::uint32_t, 2> pixel{
    0xC0000000u, 0x94200688u, // EXP_DONE PIX0, R0.xyzw, end program
};

} // namespace wiiu::gx2::shaders
