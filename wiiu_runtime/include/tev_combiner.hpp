#pragma once
#include <array>
#include <cstdint>

namespace wiiu {
class TEVCombiner {
public:
    static constexpr std::uint32_t kBase=0x1EF00C00u, kEnd=0x1EF00D00u;
    static constexpr unsigned kStageCount=6, kRowsPerStage=11;
    using Color=std::array<float,4>;
    // One-hot sources and affine modifiers keep the static Latte shader branch-free.
    // Per operand: RGB sources, alpha sources, RGB modifier, alpha modifier.
    // Rows 8/9/10: constant RGBA, RGB polynomial coefficients, alpha coefficients.
    struct alignas(16) UniformStage { std::array<Color,kRowsPerStage> rows{}; };
    struct alignas(256) UniformBlock {
        std::array<UniformStage,kStageCount> stages{};
        std::array<float,56> padding{}; // Complete the final 32-vec4 cache window.
    };
    struct Stage {
        std::array<std::uint32_t,3> rgb_sources{}, alpha_sources{}, rgb_modifiers{}, alpha_modifiers{};
        std::uint32_t rgb_operator{}, alpha_operator{}, constant{}, rgb_scale{}, alpha_scale{};
    };
    TEVCombiner() noexcept;
    // Alias layout: six 0x10-byte records (sources/modifiers/ops/scales), constants +0x60.
    bool WriteRegister(std::uint32_t address,std::uint32_t value,std::uint8_t byte_mask=15);
    [[nodiscard]] std::uint32_t ReadRegister(std::uint32_t address) const noexcept;
    [[nodiscard]] Stage GetStage(unsigned stage) const;
    // Unsupported used sources/modifiers/operators throw before draw submission.
    [[nodiscard]] UniformBlock BuildUniformBlock() const;
    [[nodiscard]] Color Evaluate(Color primary,Color texture0) const;
private:
    std::array<std::uint32_t,64> registers_{};
};
static_assert(sizeof(TEVCombiner::UniformStage)==176);
static_assert(sizeof(TEVCombiner::UniformBlock)==1280);
} // namespace wiiu
