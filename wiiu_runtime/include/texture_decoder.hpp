#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace wiiu {
enum class TextureFormat : std::uint32_t { RGBA8888 = 0, RGB565 = 3, RGBA4444 = 4 };

// MVP supports complete 8x8 tiles, dimensions 8..1024, without a vertical flip.
[[nodiscard]] std::size_t TextureSourceSize(std::uint32_t width, std::uint32_t height,
                                           TextureFormat format);
// Always outputs width*height*4 bytes in R,G,B,A order, expanding 16-bit formats.
// Raw pointer callers must provide the exact source/destination capacities.
void Deswizzle3DSTexture(const std::uint8_t* src_morton, std::uint8_t* dst_linear,
                        std::uint32_t width, std::uint32_t height, TextureFormat format);
// Checked overload validates capacities and rejects overlapping buffers.
void Deswizzle3DSTexture(std::span<const std::uint8_t> source, std::span<std::uint8_t> destination,
                        std::uint32_t width, std::uint32_t height, TextureFormat format);
} // namespace wiiu
