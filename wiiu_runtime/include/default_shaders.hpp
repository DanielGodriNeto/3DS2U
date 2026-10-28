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

// Textured input R3=UV, so the transformed position moves to R4.
inline constexpr auto textured_vertex = [] {
    std::array<std::uint32_t, 42> words{};
    for (unsigned i = 0; i < 8; ++i) words[i] = vertex[i];
    words[2] = 5; // ALU follows five CF instructions.
    words[4] = 0xC002203Cu; // POS0 = R4
    words[7] = 0x13800688u; // EXP PARAM0 color; PARAM1 completes parameter exports.
    words[8] = 0xC001C001u; words[9] = 0x14200B08u; // PARAM1 = R3.xy01, end
    for (unsigned i = 8; i < 40; ++i) {
        words[i + 2] = vertex[i];
        if ((i & 1u) != 0) words[i + 2] = (words[i + 2] & ~(0x7Fu << 21)) | (4u << 21);
    }
    return words;
}();

// PS inputs R0=color, R1=UV. Sample unit 0 into R2, multiply, export R0.
inline constexpr auto textured_pixel = [] {
    std::array<std::uint32_t, 20> words{};
    words[0] = 4; words[1] = 0x80800000u; // TEX clause, one sample
    words[2] = 6; words[3] = 0xA00C0000u; // ALU clause, four multiplies
    words[4] = 0xC0000000u; words[5] = 0x94200688u; // PIX0 = R0, end
    words[8] = 0x00010010u; // SAMPLE resource 0, coordinates R1
    words[9] = 0xF00D1002u; // R2.xyzw, normalized coordinates
    words[10] = 0xB0800000u; // sampler 0, source xy01
    for (unsigned lane = 0; lane < 4; ++lane) {
        words[12 + lane * 2] = (lane << 10) | (2u << 13) | (lane << 23) |
                               (lane == 3 ? 0x80000000u : 0u);
        words[13 + lane * 2] = (2u << 7) | 0x10u | (lane << 29); // MUL_IEEE R0, R0, R2
    }
    return words;
}();

} // namespace wiiu::gx2::shaders
