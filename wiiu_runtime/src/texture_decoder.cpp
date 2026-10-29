#include "texture_decoder.hpp"
#include <array>
#include <stdexcept>

namespace wiiu {
std::size_t TextureSourceSize(std::uint32_t width, std::uint32_t height, TextureFormat format) {
    if (width < 8 || height < 8 || width > 1024 || height > 1024 || ((width | height) & 7u) != 0)
        throw std::invalid_argument("Texture dimensions require complete 8x8 tiles, maximum 1024");
    if (format != TextureFormat::RGBA8888 && format != TextureFormat::RGB565 && format != TextureFormat::RGBA4444)
        throw std::invalid_argument("Unsupported 3DS texture format");
    return static_cast<std::size_t>(width) * height * (format == TextureFormat::RGBA8888 ? 4u : 2u);
}

void Deswizzle3DSTexture(std::span<const std::uint8_t> source, std::span<std::uint8_t> destination,
                        std::uint32_t width, std::uint32_t height, TextureFormat format) {
    const auto source_size = TextureSourceSize(width, height, format);
    const auto destination_size = static_cast<std::size_t>(width) * height * 4u;
    if (source.data() == nullptr || destination.data() == nullptr ||
        source.size() < source_size || destination.size() < destination_size)
        throw std::invalid_argument("Texture buffer capacity is insufficient");
    const auto src = reinterpret_cast<std::uintptr_t>(source.data());
    const auto dst = reinterpret_cast<std::uintptr_t>(destination.data());
    if ((src <= dst && dst - src < source_size) || (dst < src && src - dst < destination_size))
        throw std::invalid_argument("Texture source and destination overlap");
    constexpr std::array<unsigned, 8> spread{0, 1, 4, 5, 16, 17, 20, 21};
    const auto bpp = format == TextureFormat::RGBA8888 ? 4u : 2u;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto tile = static_cast<std::size_t>(y / 8u) * (width / 8u) + x / 8u;
            const auto offset = (tile * 64u + spread[x & 7u] + 2u * spread[y & 7u]) * bpp;
            const auto* p = source.data() + offset;
            auto* out = destination.data() + (static_cast<std::size_t>(y) * width + x) * 4u;
            if (format == TextureFormat::RGBA8888) {
                out[0] = p[3]; out[1] = p[2]; out[2] = p[1]; out[3] = p[0];
            } else {
                const auto value = std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8);
                if (format == TextureFormat::RGB565) {
                    const auto r = (value >> 11) & 31u, g = (value >> 5) & 63u, b = value & 31u;
                    out[0] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
                    out[1] = static_cast<std::uint8_t>((g << 2) | (g >> 4));
                    out[2] = static_cast<std::uint8_t>((b << 3) | (b >> 2)); out[3] = 255;
                } else {
                    for (unsigned c = 0; c < 4; ++c)
                        out[c] = static_cast<std::uint8_t>(((value >> ((3u - c) * 4u)) & 15u) * 17u);
                }
            }
        }
    }
}

void Deswizzle3DSTexture(const std::uint8_t* source, std::uint8_t* destination,
                        std::uint32_t width, std::uint32_t height, TextureFormat format) {
    const auto source_size = TextureSourceSize(width, height, format);
    if (source == nullptr || destination == nullptr) throw std::invalid_argument("Null texture buffer");
    Deswizzle3DSTexture(std::span{source, source_size},
                       std::span{destination, static_cast<std::size_t>(width) * height * 4u},
                       width, height, format);
}
} // namespace wiiu
