#pragma once

#if __has_include(<gx2/init.h>)
#include <gx2/init.h>
#else
#include <gx2/state.h> // wut declares GX2Init/GX2Shutdown here.
#endif
#include <gx2/context.h>
#include <gx2/display.h>
#include <gx2/shaders.h>
#include <coreinit/memheap.h>

#include <cstdint>
#include <array>
#include <vector>

namespace wiiu::gx2 {

// Exclusive GX2 owner; use on the initialization core while in the foreground.
// Shutdown must precede foreground release; ProcUI integration belongs to the app.
class GX2Manager {
public:
    enum class Output { TV, DRC };

    GX2Manager() = default;
    ~GX2Manager() noexcept;
    GX2Manager(const GX2Manager&) = delete;
    GX2Manager& operator=(const GX2Manager&) = delete;
    GX2Manager(GX2Manager&&) = delete;
    GX2Manager& operator=(GX2Manager&&) = delete;

    [[nodiscard]] bool Initialize();
    void BeginFrame(); // Clears both outputs and selects the TV render target.
    void SetClearColor(float r, float g, float b, float a) noexcept;
    // Guest vertices are LE float3 position + float4 color; stride may include padding.
    // Requires an active frame. Uploads remain owned until GPU completion.
    void DrawTriangles(const void* vtx_data, std::uint32_t count, std::uint32_t stride);
    // Count is scalar floats (a multiple of four), maximum 96 vec4s.
    // Register mode: row-major modelview/projection matrix occupies c0-c3.
    void SetVertexUniforms(const float* uniform_data, std::uint32_t count);
    void BindOutput(Output output); // Select TV or DRC for subsequent GX2 draws.
    void EndFrame();
    void Shutdown() noexcept;

private:
    struct Target {
        GX2ColorBuffer color{};
        GX2DepthBuffer depth{};
        GX2ContextState* context{};
        void* scan{};
        std::uint32_t scan_size{};
    };

    [[nodiscard]] bool AllocateTarget(Target& target, Output output);
    static void ReleaseTarget(Target& target) noexcept;
    [[nodiscard]] bool InitializeShaders();
    void FreeUploads() noexcept;

    Target tv_{};
    Target drc_{};
    std::array<float, 4> clear_color_{0.0f, 0.0f, 0.0f, 1.0f};
    GX2VertexShader vertex_shader_{};
    GX2PixelShader pixel_shader_{};
    GX2FetchShader fetch_shader_{};
    std::array<float, 384> vertex_uniforms_ = [] {
        std::array<float, 384> values{};
        for (unsigned i = 0; i < 4; ++i) values[i * 5] = 1.0f;
        return values;
    }();
    GX2UniformVar matrix_uniform_{"modelview_projection", GX2_SHADER_VAR_TYPE_FLOAT4X4, 1, 0, -1};
    std::vector<void*> frame_uploads_;
    bool gx2_started_{};
    bool initialized_{};
    bool frame_active_{};
    bool presented_{};
};

} // namespace wiiu::gx2
