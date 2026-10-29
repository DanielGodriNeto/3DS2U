#pragma once

#include <cstdint>

namespace wiiu {
class WiiUMemory;
namespace gx2 { class GX2Manager; }

// GSP pixel-transfer engine; guest RGB8/RGBA8 bytes are BGR/ABGR respectively.
class DisplayTransfer {
public:
    DisplayTransfer() = default;
    explicit DisplayTransfer(gx2::GX2Manager& graphics) noexcept : graphics_(&graphics) {}
    // Dimensions pack width in bits 0..15 and height in bits 16..31.
    // Rejects unsupported flags, mismatched dimensions and incomplete Morton tiles.
    void ExecuteTransfer(WiiUMemory& memory, std::uint32_t src_addr, std::uint32_t dst_addr,
                         std::uint32_t src_dim, std::uint32_t dst_dim, std::uint32_t flags);
    // Explicit scanout destinations override the MVP dimension-based screen selection.
    enum class Screen { Top, Bottom };
    void SetFramebuffer(Screen screen, std::uint32_t address) noexcept;
private:
    gx2::GX2Manager* graphics_{};
    std::uint32_t top_address_{}, bottom_address_{};
};
} // namespace wiiu
