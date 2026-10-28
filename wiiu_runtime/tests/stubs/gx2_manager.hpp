#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace wiiu::gx2 {

// Host test double; production builds use wiiu_runtime/include/gx2_manager.hpp.
class GX2Manager {
public:
    void SetClearColor(float r, float g, float b, float a) noexcept {
        color = {r, g, b, a};
        ++writes;
    }
    void DrawTriangles(const void* vertices, std::uint32_t count, std::uint32_t stride) {
        const auto* bytes = static_cast<const std::uint8_t*>(vertices);
        vertex_bytes.assign(bytes, bytes + static_cast<std::size_t>(count) * stride);
        vertex_count = count;
        vertex_stride = stride;
        ++draws;
    }

    std::array<float, 4> color{};
    unsigned writes{};
    unsigned draws{};
    std::uint32_t vertex_count{}, vertex_stride{};
    std::vector<std::uint8_t> vertex_bytes;
};

} // namespace wiiu::gx2
