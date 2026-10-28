#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <array>
#include "texture_decoder.hpp"

namespace wiiu {
class WiiUMemory;
namespace gx2 {
class GX2Manager;
}

// MVP register bridge; complete PICA200 command decoding remains separate.
class Pica200 {
public:
    static constexpr std::uint32_t kMMIOBase = 0x1EF00000u;
    static constexpr std::uint32_t kMMIOEnd = 0x1EFFFFFFu; // Exclusive intercept limit.
    static constexpr std::uint32_t kClearColorRegister = kMMIOBase;
    static constexpr std::uint32_t kVertexBufferRegister = 0x1EF00200u;
    static constexpr std::uint32_t kVertexStrideRegister = 0x1EF00204u;
    static constexpr std::uint32_t kVertexCountRegister = 0x1EF00208u;
    static constexpr std::uint32_t kDrawRegister = 0x1EF0020Cu;
    static constexpr std::uint32_t kUniformBase = 0x1EF00280u;
    static constexpr std::uint32_t kUniformFloatCount = 96u * 4u;
    static constexpr std::uint32_t kUniformEnd = kUniformBase + kUniformFloatCount * 4u;
    using VertexUniforms = std::array<float, kUniformFloatCount>;
    static constexpr std::uint32_t kTextureAddress = 0x1EF00B00u;
    static constexpr std::uint32_t kTextureDimensions = 0x1EF00B04u;
    static constexpr std::uint32_t kTextureFormat = 0x1EF00B0Cu;

    // The graphics manager must outlive this non-owning bridge.
    explicit Pica200(gx2::GX2Manager& manager) noexcept : gx2_manager_(manager) {}
    void AttachMemory(WiiUMemory& memory) noexcept { memory_ = &memory; }

    // Values are guest register integers, before any FCRAM byte swapping.
    void WriteMMIO(std::uint32_t address, std::uint32_t value, std::uint8_t byte_mask = 0xF);
    [[nodiscard]] std::uint32_t GetRegisterValue(std::uint32_t address) const noexcept;
    [[nodiscard]] VertexUniforms GetVertexUniforms() const noexcept;
    void BeginCommandList(WiiUMemory& memory);
    // Validates queued geometry, then submits snapshots in guest draw order.
    [[nodiscard]] bool EndCommandList();
    void CancelCommandList() noexcept;

private:
    struct TextureState { std::uint32_t address{}, dimensions{}, format{}; };
    struct DrawRequest {
        std::uint32_t address, stride, count;
        VertexUniforms uniforms;
        TextureState texture;
    };
    [[nodiscard]] const void* ValidateDraw(const DrawRequest& request) const;
    void SubmitDraw(const DrawRequest& request);
    gx2::GX2Manager& gx2_manager_;
    WiiUMemory* memory_{};
    std::uint32_t vbo_addr_{};
    std::uint32_t vbo_stride_{28}; // MVP: LE float3 position followed by LE float4 color.
    std::uint32_t vertex_count_{};
    std::unordered_map<std::uint32_t, std::uint32_t> registers_{{kVertexStrideRegister, 28u}};
    std::vector<DrawRequest> pending_draws_;
    bool batching_{};
    TextureState texture0_{};
    // MVP direct scalar register bank; first four vec4s form a row-major mat4.
    std::array<std::uint32_t, kUniformFloatCount> uniforms24_ = [] {
        std::array<std::uint32_t, kUniformFloatCount> values{};
        for (unsigned i = 0; i < 4; ++i) values[i * 5] = 0x3F0000u;
        return values;
    }();
};

} // namespace wiiu
