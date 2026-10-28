#pragma once

#if __has_include(<gx2/init.h>)
#include <gx2/init.h>
#else
#include <gx2/state.h> // wut declares GX2Init/GX2Shutdown here.
#endif
#include <gx2/context.h>
#include <gx2/display.h>
#include <coreinit/memheap.h>

#include <cstdint>
#include <array>

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

    Target tv_{};
    Target drc_{};
    std::array<float, 4> clear_color_{0.0f, 0.0f, 0.0f, 1.0f};
    bool gx2_started_{};
    bool initialized_{};
    bool frame_active_{};
    bool presented_{};
};

} // namespace wiiu::gx2
