#include "display_transfer.hpp"
#include "wiiu_memory.hpp"
#include <gx2_manager.hpp>

#include <array>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace wiiu {
namespace {
using U32 = std::uint32_t;
using Pixel = std::array<std::uint8_t, 4>;
U32 BytesPerPixel(U32 format) {
    switch (format) {
    case 0: return 4;
    case 1: return 3;
    case 2: return 2;
    default: throw std::invalid_argument("Unsupported display-transfer pixel format");
    }
}
std::size_t PixelOffset(U32 x, U32 y, U32 width, bool linear, U32 bpp) {
    if (linear) return (static_cast<std::size_t>(y) * width + x) * bpp;
    constexpr std::array<unsigned, 8> spread{0, 1, 4, 5, 16, 17, 20, 21};
    const auto tile = static_cast<std::size_t>(y / 8u) * (width / 8u) + x / 8u;
    return (tile * 64u + spread[x & 7u] + 2u * spread[y & 7u]) * bpp;
}
Pixel Decode(const std::uint8_t* p, U32 format) {
    if (format == 0) return {p[3], p[2], p[1], p[0]};
    if (format == 1) return {p[2], p[1], p[0], 255};
    const auto word = U32{p[0]} | (U32{p[1]} << 8);
    const auto r = (word >> 11) & 31u, g = (word >> 5) & 63u, b = word & 31u;
    return {static_cast<std::uint8_t>((r << 3) | (r >> 2)),
            static_cast<std::uint8_t>((g << 2) | (g >> 4)),
            static_cast<std::uint8_t>((b << 3) | (b >> 2)), 255};
}
void Encode(std::uint8_t* p, U32 format, const Pixel& color) {
    if (format == 0) { p[0] = color[3]; p[1] = color[2]; p[2] = color[1]; p[3] = color[0]; }
    else if (format == 1) { p[0] = color[2]; p[1] = color[1]; p[2] = color[0]; }
    else {
        const U32 word = ((U32{color[0]} >> 3) << 11) | ((U32{color[1]} >> 2) << 5) | (color[2] >> 3);
        p[0] = static_cast<std::uint8_t>(word); p[1] = static_cast<std::uint8_t>(word >> 8);
    }
}
} // namespace

void DisplayTransfer::SetFramebuffer(Screen screen, U32 address) noexcept {
    (screen == Screen::Top ? top_address_ : bottom_address_) = address;
}

void DisplayTransfer::ExecuteTransfer(WiiUMemory& memory, U32 source, U32 destination,
                                      U32 src_dim, U32 dst_dim, U32 flags) {
    const auto width = src_dim & 0xFFFFu, height = src_dim >> 16;
    const auto out_width = dst_dim & 0xFFFFu, out_height = dst_dim >> 16;
    // Flip, linear input, don't-swizzle and format fields. AA/raw/crop/32x32 are deferred.
    constexpr U32 supported = 0x00007723u;
    if ((flags & ~supported) != 0 || width == 0 || height == 0 || width > 1024 || height > 1024 ||
        width != out_width || height != out_height)
        throw std::invalid_argument("Unsupported display-transfer flags or dimensions");
    const auto in_format = (flags >> 8) & 7u, out_format = (flags >> 12) & 7u;
    const auto in_bpp = BytesPerPixel(in_format), out_bpp = BytesPerPixel(out_format);
    const bool in_linear = (flags & 2u) != 0;
    const bool out_linear = in_linear == ((flags & 0x20u) != 0);
    if ((!in_linear || !out_linear) && ((width | height) & 7u) != 0)
        throw std::invalid_argument("Morton display transfers require complete 8x8 tiles");
    const auto pixels = static_cast<std::size_t>(width) * height;
    const auto* src = static_cast<const std::uint8_t*>(memory.GetPointer(source, pixels * in_bpp));
    (void)memory.GetPointer(destination, pixels * out_bpp);
    // Stage the whole result so overlapping buffers and failed validation are safe.
    std::vector<std::uint8_t> result(pixels * out_bpp);
    const bool landscape_top = width == 400 && height == 240;
    const bool landscape_bottom = width == 320 && height == 240;
    const bool rotated_top = width == 240 && height == 400;
    const bool rotated_bottom = width == 240 && height == 320;
    const bool top = out_linear && (landscape_top || rotated_top) &&
        (top_address_ == 0 || top_address_ == destination);
    const bool bottom = out_linear && (landscape_bottom || rotated_bottom) &&
        (bottom_address_ == 0 || bottom_address_ == destination);
    const bool present = graphics_ != nullptr && (top || bottom);
    const U32 screen_width = top ? 400u : 320u;
    const bool rotate = rotated_top || rotated_bottom;
    std::vector<std::uint8_t> rgba(present ? pixels * 4u : 0);
    for (U32 y = 0; y < height; ++y) {
        const auto out_y = (flags & 1u) != 0 ? height - 1u - y : y;
        for (U32 x = 0; x < width; ++x) {
            auto color = Decode(src + PixelOffset(x, y, width, in_linear, in_bpp), in_format);
            auto* dst = result.data() + PixelOffset(x, out_y, width, out_linear, out_bpp);
            Encode(dst, out_format, color);
            if (present) {
                // Present the converted destination, including any output quantization.
                color = Decode(dst, out_format);
                const auto screen_x = rotate ? out_y : x;
                const auto screen_y = rotate ? width - 1u - x : out_y;
                auto* pixel = rgba.data() + (static_cast<std::size_t>(screen_y) * screen_width + screen_x) * 4u;
                for (unsigned c = 0; c < 4; ++c) pixel[c] = color[c];
            }
        }
    }
    // QueueDisplayBuffer owns its snapshot; later FCRAM writes cannot change this frame.
    if (present) graphics_->QueueDisplayBuffer(top ? gx2::GX2Manager::Output::TV : gx2::GX2Manager::Output::DRC,
                                              rgba, screen_width, 240);
    memory.LoadImage(destination, result);
    if (present) memory.RequestFrame();
}
} // namespace wiiu
