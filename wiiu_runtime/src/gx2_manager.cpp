#include "gx2_manager.hpp"

#include <coreinit/debug.h>
#include <coreinit/memdefaultheap.h>
#include <gx2/clear.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/state.h>
#include <gx2/swap.h>

#include <cstring>
#include <initializer_list>
#include <new>

namespace wiiu::gx2 {
namespace {

constexpr auto kFormat = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
constexpr auto kBuffering = GX2_BUFFERING_MODE_DOUBLE;
constexpr auto kTVMode = GX2_TV_RENDER_MODE_WIDE_720P;
constexpr auto kDRCMode = GX2_DRC_RENDER_MODE_SINGLE;

// Flush initial CPU writes before GX2 can use scan and render allocations.
void* Allocate(std::uint32_t size, std::uint32_t alignment) {
    if (size == 0 || alignment == 0) {
        return nullptr;
    }
    void* data = MEMAllocFromDefaultHeapEx(size, static_cast<std::int32_t>(alignment));
    if (data != nullptr) {
        std::memset(data, 0, size);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU, data, size);
    }
    return data;
}

} // namespace

GX2Manager::~GX2Manager() noexcept {
    Shutdown();
}

bool GX2Manager::AllocateTarget(Target& target, Output output) {
    const bool tv = output == Output::TV;
    const std::uint32_t width = tv ? 1280u : 854u;
    const std::uint32_t height = tv ? 720u : 480u;
    std::uint32_t unused = 0;
    if (tv) {
        GX2CalcTVSize(kTVMode, kFormat, kBuffering, &target.scan_size, &unused);
    } else {
        GX2CalcDRCSize(kDRCMode, kFormat, kBuffering, &target.scan_size, &unused);
    }
    target.scan = Allocate(target.scan_size, GX2_SCAN_BUFFER_ALIGNMENT);
    if (target.scan == nullptr) {
        return false;
    }

    auto& color = target.color;
    color.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    color.surface.width = width;
    color.surface.height = height;
    color.surface.depth = 1;
    color.surface.mipLevels = 1;
    color.surface.format = kFormat;
    color.surface.aa = GX2_AA_MODE1X;
    color.surface.use = GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
    color.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    color.viewNumSlices = 1;
    GX2CalcSurfaceSizeAndAlignment(&color.surface);
    color.surface.image = Allocate(color.surface.imageSize, color.surface.alignment);
    if (color.surface.image == nullptr) {
        return false;
    }
    GX2InitColorBufferRegs(&color);

    auto& depth = target.depth;
    depth.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    depth.surface.width = width;
    depth.surface.height = height;
    depth.surface.depth = 1;
    depth.surface.mipLevels = 1;
    depth.surface.format = GX2_SURFACE_FORMAT_FLOAT_R32;
    depth.surface.aa = GX2_AA_MODE1X;
    depth.surface.use = GX2_SURFACE_USE_DEPTH_BUFFER | GX2_SURFACE_USE_TEXTURE;
    depth.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    depth.viewNumSlices = 1;
    depth.depthClear = 1.0f;
    GX2CalcSurfaceSizeAndAlignment(&depth.surface);
    depth.surface.image = Allocate(depth.surface.imageSize, depth.surface.alignment);
    if (depth.surface.image == nullptr) {
        return false;
    }
    GX2InitDepthBufferRegs(&depth);

    void* context = Allocate(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT);
    if (context == nullptr) {
        return false;
    }
    target.context = ::new (context) GX2ContextState{};
    GX2SetupContextStateEx(target.context, TRUE);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, target.context, sizeof(GX2ContextState));
    return true;
}

bool GX2Manager::Initialize() {
    if (initialized_) {
        return true;
    }
    if (GX2GetMainCoreId() >= 0) {
        OSReport("[3DS2U GX2] GX2 is already owned by another graphics initializer\n");
        return false;
    }

    GX2Init(nullptr);
    if (GX2GetMainCoreId() < 0) {
        OSReport("[3DS2U GX2] GX2 initialization failed\n");
        return false;
    }
    gx2_started_ = true;
    GX2SetTVEnable(FALSE);
    GX2SetDRCEnable(FALSE);

    if (!AllocateTarget(tv_, Output::TV) || !AllocateTarget(drc_, Output::DRC)) {
        OSReport("[3DS2U GX2] Failed to allocate aligned graphics resources\n");
        Shutdown();
        return false;
    }

    GX2SetTVBuffer(tv_.scan, tv_.scan_size, kTVMode, kFormat, kBuffering);
    GX2SetDRCBuffer(drc_.scan, drc_.scan_size, kDRCMode, kFormat, kBuffering);
    GX2SetTVScale(1280, 720);
    GX2SetDRCScale(854, 480);
    GX2SetSwapInterval(1);
    initialized_ = true;
    return true;
}

void GX2Manager::BindOutput(Output output) {
    if (!initialized_ || !frame_active_) {
        return;
    }
    auto& target = output == Output::TV ? tv_ : drc_;
    GX2SetContextState(target.context);
    GX2SetColorBuffer(&target.color, GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(&target.depth);
    GX2SetViewport(0.0f, 0.0f, static_cast<float>(target.color.surface.width),
                   static_cast<float>(target.color.surface.height), 0.0f, 1.0f);
    GX2SetScissor(0, 0, target.color.surface.width, target.color.surface.height);
}

void GX2Manager::SetClearColor(float r, float g, float b, float a) noexcept {
    clear_color_ = {r, g, b, a};
}

void GX2Manager::BeginFrame() {
    if (!initialized_ || frame_active_) {
        return;
    }
    // GPU completion alone does not retire a pending display-buffer flip.
    if (presented_) {
        GX2WaitForFlip();
    }
    frame_active_ = true;
    for (const auto output : {Output::TV, Output::DRC}) {
        auto& target = output == Output::TV ? tv_ : drc_;
        BindOutput(output);
        GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER,
                      target.color.surface.image, target.color.surface.imageSize);
        GX2Invalidate(GX2_INVALIDATE_MODE_DEPTH_BUFFER,
                      target.depth.surface.image, target.depth.surface.imageSize);
        GX2ClearColor(&target.color, clear_color_[0], clear_color_[1],
                      clear_color_[2], clear_color_[3]);
        GX2ClearDepthStencilEx(&target.depth, 1.0f, 0, GX2_CLEAR_FLAGS_DEPTH);
        // GX2 clear operations may alter registers in the active context.
        GX2SetContextState(target.context);
    }
    BindOutput(Output::TV);
}

void GX2Manager::EndFrame() {
    if (!initialized_ || !frame_active_) {
        return;
    }
    GX2CopyColorBufferToScanBuffer(&tv_.color, GX2_SCAN_TARGET_TV);
    GX2CopyColorBufferToScanBuffer(&drc_.color, GX2_SCAN_TARGET_DRC);
    GX2SwapScanBuffers();
    GX2Flush();
    GX2DrawDone();
    // Enable scanout only once both outputs contain a completed frame.
    GX2SetTVEnable(TRUE);
    GX2SetDRCEnable(TRUE);
    frame_active_ = false;
    presented_ = true;
}

void GX2Manager::ReleaseTarget(Target& target) noexcept {
    if (target.context != nullptr) {
        target.context->~GX2ContextState();
        MEMFreeToDefaultHeap(target.context);
    }
    if (target.color.surface.image != nullptr) {
        MEMFreeToDefaultHeap(target.color.surface.image);
    }
    if (target.depth.surface.image != nullptr) {
        MEMFreeToDefaultHeap(target.depth.surface.image);
    }
    if (target.scan != nullptr) {
        MEMFreeToDefaultHeap(target.scan);
    }
    target = {};
}

void GX2Manager::Shutdown() noexcept {
    if (gx2_started_) {
        GX2Flush();
        GX2DrawDone();
        if (presented_) {
            GX2WaitForFlip();
        }
        GX2SetTVEnable(FALSE);
        GX2SetDRCEnable(FALSE);
        GX2Shutdown();
    }
    // GX2 must stop referencing surfaces and contexts before their storage is freed.
    ReleaseTarget(drc_);
    ReleaseTarget(tv_);
    gx2_started_ = initialized_ = frame_active_ = presented_ = false;
}

} // namespace wiiu::gx2
