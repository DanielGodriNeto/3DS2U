#pragma once

#include <cstdint>

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

    // The graphics manager must outlive this non-owning bridge.
    explicit Pica200(gx2::GX2Manager& manager) noexcept : gx2_manager_(manager) {}
    void AttachMemory(WiiUMemory& memory) noexcept { memory_ = &memory; }

    // Values are guest register integers, before any FCRAM byte swapping.
    void WriteMMIO(std::uint32_t address, std::uint32_t value);

private:
    gx2::GX2Manager& gx2_manager_;
    WiiUMemory* memory_{};
    std::uint32_t vbo_addr_{};
    std::uint32_t vbo_stride_{28}; // MVP: LE float3 position followed by LE float4 color.
    std::uint32_t vertex_count_{};
};

} // namespace wiiu
