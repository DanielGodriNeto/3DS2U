#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include <functional>

enum GX2SurfaceFormat { GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8 = 0x1A };

namespace wiiu::gx2 {

// Host test double; production builds use wiiu_runtime/include/gx2_manager.hpp.
class GX2Manager {
public:
    void BindTexture0(const std::uint8_t* data, std::uint32_t width, std::uint32_t height, GX2SurfaceFormat) {
        texture.assign(data, data + static_cast<std::size_t>(width) * height * 4u);
        texture_width = width; texture_height = height; texture_enabled = true;
    }
    void DisableTexture0() noexcept { texture_enabled = false; }
    void SetVertexUniforms(const float* data, std::uint32_t count) {
        uniforms.assign(data, data + count);
        uniform_history.push_back(uniforms);
    }
    void SetClearColor(float r, float g, float b, float a) noexcept {
        color = {r, g, b, a};
        ++writes;
    }
    void DrawTriangles(const void* vertices, std::uint32_t count, std::uint32_t stride) {
        if (before_draw) before_draw();
        const auto* bytes = static_cast<const std::uint8_t*>(vertices);
        vertex_bytes.assign(bytes, bytes + static_cast<std::size_t>(count) * stride);
        vertex_count = count;
        vertex_stride = stride;
        ++draws;
        draw_counts.push_back(count);
        texture_history.push_back(texture_enabled ? texture : std::vector<std::uint8_t>{});
    }

    std::array<float, 4> color{};
    unsigned writes{};
    unsigned draws{};
    std::uint32_t vertex_count{}, vertex_stride{};
    std::vector<std::uint8_t> vertex_bytes;
    std::vector<std::uint32_t> draw_counts;
    std::function<void()> before_draw;
    std::vector<float> uniforms;
    std::vector<std::vector<float>> uniform_history;
    std::vector<std::uint8_t> texture;
    std::vector<std::vector<std::uint8_t>> texture_history;
    std::uint32_t texture_width{}, texture_height{};
    bool texture_enabled{};
};

} // namespace wiiu::gx2
