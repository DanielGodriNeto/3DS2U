#include "texture_decoder.hpp"
#include "pica200.hpp"
#include "gsp_gpu.hpp"
#include "wiiu_memory.hpp"
#include "gx2_manager.hpp"
#include "default_shaders.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void Check(bool value) { if (!value) throw std::runtime_error("Texture regression"); }
template<class Operation> void Reject(Operation operation) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    Check(rejected);
}
using Pixel = std::array<std::uint8_t, 4>;
Pixel Checker(unsigned x, unsigned y) {
    return ((x / 2 + y / 2 + x / 8) & 1u) == 0 ?
        Pixel{0xE1, 0x22, 0x43, 0xA4} : Pixel{0x19, 0xCA, 0x6B, 0x5C};
}
// Construct the source independently, by deinterleaving tile-local pixel indices.
template<class Encode> std::vector<std::uint8_t> Morton(unsigned width, unsigned height,
                                                       unsigned bpp, Encode encode) {
    std::vector<std::uint8_t> source(static_cast<std::size_t>(width) * height * bpp);
    for (unsigned ty = 0; ty < height / 8; ++ty)
        for (unsigned tx = 0; tx < width / 8; ++tx)
            for (unsigned index = 0; index < 64; ++index) {
                unsigned x = 0, y = 0;
                for (unsigned bit = 0; bit < 3; ++bit) {
                    x |= ((index >> (bit * 2)) & 1u) << bit;
                    y |= ((index >> (bit * 2 + 1)) & 1u) << bit;
                }
                encode(source.data() + ((ty * (width / 8) + tx) * 64 + index) * bpp,
                       tx * 8 + x, ty * 8 + y);
            }
    return source;
}
void CheckPixel(const std::vector<std::uint8_t>& bytes, unsigned width, unsigned x, unsigned y, Pixel expected) {
    Check(std::equal(expected.begin(), expected.end(), bytes.begin() + (y * width + x) * 4));
}
} // namespace

int main() {
    using Format = wiiu::TextureFormat;
    auto source = Morton(16, 16, 4, [](auto* out, auto x, auto y) {
        const auto rgba = Checker(x, y);
        for (unsigned c = 0; c < 4; ++c) out[c] = rgba[3 - c];
    });
    std::vector<std::uint8_t> decoded(16 * 16 * 4 + 8, 0xCD);
    wiiu::Deswizzle3DSTexture(source, decoded, 16, 16, Format::RGBA8888);
    for (const auto& xy : {std::array<unsigned, 2>{0, 0}, {7, 7}, {8, 0}})
        CheckPixel(decoded, 16, xy[0], xy[1], Checker(xy[0], xy[1]));
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x) CheckPixel(decoded, 16, x, y, Checker(x, y));
    for (unsigned i = 1024; i < decoded.size(); ++i) Check(decoded[i] == 0xCD);

    auto gradient = Morton(24, 16, 4, [](auto* out, auto x, auto y) {
        out[0] = 255; out[1] = static_cast<std::uint8_t>(x + y);
        out[2] = static_cast<std::uint8_t>(y); out[3] = static_cast<std::uint8_t>(x);
    });
    std::vector<std::uint8_t> gradient_out(24 * 16 * 4);
    wiiu::Deswizzle3DSTexture(gradient.data(), gradient_out.data(), 24, 16, Format::RGBA8888);
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 24; ++x)
            CheckPixel(gradient_out, 24, x, y, {static_cast<std::uint8_t>(x),
                static_cast<std::uint8_t>(y), static_cast<std::uint8_t>(x + y), 255});

    for (auto format : {Format::RGB565, Format::RGBA4444}) {
        auto packed = Morton(8, 8, 2, [](auto* out, auto x, auto y) {
            const auto value = 0x1234u + x * 0x100u + y;
            out[0] = static_cast<std::uint8_t>(value); out[1] = static_cast<std::uint8_t>(value >> 8);
        });
        std::vector<std::uint8_t> output(8 * 8 * 4);
        wiiu::Deswizzle3DSTexture(packed, output, 8, 8, format);
        for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
            const auto value = 0x1234u + x * 0x100u + y;
            Pixel expected{};
            if (format == Format::RGB565) {
                const auto r = value / 2048, g = (value / 32) % 64, b = value % 32;
                expected = {static_cast<std::uint8_t>(r * 8 + r / 4),
                    static_cast<std::uint8_t>(g * 4 + g / 16), static_cast<std::uint8_t>(b * 8 + b / 4), 255};
            } else for (unsigned c = 0; c < 4; ++c)
                expected[c] = static_cast<std::uint8_t>(((value >> ((3 - c) * 4)) & 15u) * 17u);
            CheckPixel(output, 8, x, y, expected);
        }
    }
    Reject([&] { wiiu::Deswizzle3DSTexture(std::span{source}.first(1023), std::span{decoded}, 16, 16, Format::RGBA8888); });
    Reject([&] { wiiu::Deswizzle3DSTexture(std::span{source}, std::span{decoded}.first(1023), 16, 16, Format::RGBA8888); });
    Reject([&] { wiiu::Deswizzle3DSTexture(source.data(), source.data(), 16, 16, Format::RGBA8888); });
    Reject([&] { wiiu::Deswizzle3DSTexture(nullptr, decoded.data(), 16, 16, Format::RGBA8888); });
    for (unsigned width : {0u, 7u, 15u, 1032u})
        Reject([&] { (void)wiiu::TextureSourceSize(width, 16, Format::RGBA8888); });
    Reject([&] { (void)wiiu::TextureSourceSize(16, 16, static_cast<Format>(99)); });

    wiiu::gx2::GX2Manager graphics;
    wiiu::Pica200 pica(graphics);
    wiiu::WiiUMemory memory(pica);
    wiiu::GSPGPU gsp(pica);
    memory.LoadImage(0x08002000u, source);
    pica.WriteMMIO(wiiu::Pica200::kVertexBufferRegister, 0x08001000u);
    pica.WriteMMIO(wiiu::Pica200::kVertexStrideRegister, 36);
    pica.WriteMMIO(wiiu::Pica200::kVertexCountRegister, 3);
    pica.WriteMMIO(wiiu::Pica200::kTextureAddress, 0x08002000u);
    pica.WriteMMIO(wiiu::Pica200::kTextureDimensions, (16u << 16) | 16u);
    pica.WriteMMIO(wiiu::Pica200::kTextureFormat, 0);
    // Draw, disable texture, draw: queued requests must retain their own texture state.
    const std::uint32_t commands[]{1, 0x000F0083u, 0, 0x000F02C0u, 1, 0x000F0083u};
    for (unsigned i = 0; i < 6; ++i) memory.Write32(0x08000000u + i * 4, commands[i]);
    gsp.ProcessCommandBuffer(memory, 0x08000000u, sizeof(commands));
    Check(graphics.draws == 2 && graphics.texture_history.size() == 2);
    Check(graphics.texture_history[0].size() == 1024 && graphics.texture_history[1].empty());
    Check(std::equal(graphics.texture_history[0].begin(), graphics.texture_history[0].end(), decoded.begin()));
    pica.WriteMMIO(wiiu::Pica200::kTextureAddress, 0x0800FFF0u);
    Reject([&] { pica.WriteMMIO(wiiu::Pica200::kDrawRegister, 1); });
    Check(graphics.draws == 2);

    // Check texture sample operands and simulate the actual four multiply instructions.
    const auto& vs = wiiu::gx2::shaders::textured_vertex;
    Check(vs[2] == 5 && ((vs[4] >> 15) & 127u) == 4);
    Check((vs[8] & 8191u) == 1 && ((vs[8] >> 15) & 127u) == 3);
    for (unsigned instruction = 0; instruction < 16; ++instruction)
        Check(((vs[11 + instruction * 2] >> 21) & 127u) == 4);
    const auto& ps = wiiu::gx2::shaders::textured_pixel;
    Check((ps[8] & 31u) == 0x10 && ((ps[8] >> 16) & 127u) == 1);
    Check((ps[9] & 127u) == 2 && ((ps[10] >> 15) & 31u) == 0);
    const std::array<float, 4> color{0.5f, 0.25f, 0.75f, 1};
    const std::array<float, 4> texel{0.2f, 0.4f, 0.8f, 0.5f};
    std::array<float, 4> result{};
    for (unsigned lane = 0; lane < 4; ++lane) {
        const auto a = ps[12 + lane * 2], b = ps[13 + lane * 2];
        Check((a & 511u) == 0 && ((a >> 13) & 511u) == 2);
        Check(((b >> 7) & 2047u) == 2 && (b & 16u) != 0);
        result[(b >> 29) & 3u] = color[(a >> 10) & 3u] * texel[(a >> 23) & 3u];
    }
    for (unsigned lane = 0; lane < 4; ++lane) Check(result[lane] == color[lane] * texel[lane]);
    std::cout << "Texture Morton/RGBA/packed-format, bounds, batch and shader tests passed.\n";
}
