#include "gx2_manager.hpp"
#include "default_shaders.hpp"

#include <coreinit/debug.h>
#include <coreinit/memdefaultheap.h>
#include <gx2/clear.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/state.h>
#include <gx2/swap.h>
#include <gx2/draw.h>
#include <gx2/utils.h>

#include <cstring>
#include <initializer_list>
#include <new>
#include <span>
#include <limits>
#include <stdexcept>
#include <algorithm>

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

bool GX2Manager::InitializeShaders() {
    const auto program = [](std::span<const std::uint32_t> words) -> void* {
        auto* bytes = static_cast<std::uint8_t*>(Allocate(256, GX2_SHADER_PROGRAM_ALIGNMENT));
        if (bytes == nullptr) return nullptr;
        for (std::size_t i = 0; i < words.size(); ++i) {
            for (unsigned j = 0; j < 4; ++j) bytes[i * 4 + j] = static_cast<std::uint8_t>(words[i] >> (j * 8));
        }
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, bytes, 256);
        return bytes;
    };
    vertex_shader_.program = program(shaders::vertex);
    pixel_shader_.program = program(shaders::pixel);
    if (vertex_shader_.program == nullptr || pixel_shader_.program == nullptr) return false;
    vertex_shader_.size = pixel_shader_.size = 256;
    vertex_shader_.mode = pixel_shader_.mode = GX2_SHADER_MODE_UNIFORM_REGISTER;
    vertex_shader_.regs.sq_pgm_resources_vs = 4u | (1u << 8);
    vertex_shader_.uniformVarCount = 1;
    vertex_shader_.uniformVars = &matrix_uniform_;
    vertex_shader_.regs.num_spi_vs_out_id = 1;
    vertex_shader_.regs.spi_vs_out_id[0] = 0xFFFFFF00u;
    vertex_shader_.regs.sq_vtx_semantic_clear = 0xFFFFFFFCu;
    vertex_shader_.regs.num_sq_vtx_semantic = 2;
    vertex_shader_.regs.sq_vtx_semantic[0] = 0;
    vertex_shader_.regs.sq_vtx_semantic[1] = 1;
    vertex_shader_.regs.vgt_vertex_reuse_block_cntl = 14;
    vertex_shader_.regs.vgt_hos_reuse_depth = 16;
    pixel_shader_.regs.sq_pgm_resources_ps = 1;
    pixel_shader_.regs.sq_pgm_exports_ps = 2;
    pixel_shader_.regs.spi_ps_in_control_0 = 1u | (1u << 28);
    pixel_shader_.regs.num_spi_ps_input_cntl = 1;
    pixel_shader_.regs.spi_ps_input_cntls[0] = 0x100u;
    pixel_shader_.regs.cb_shader_mask = 0xFu;
    pixel_shader_.regs.cb_shader_control = 1;
    pixel_shader_.regs.db_shader_control = 0x10u;

    textured_vertex_shader_ = vertex_shader_;
    textured_pixel_shader_ = pixel_shader_;
    textured_vertex_shader_.program = program(shaders::textured_vertex);
    textured_pixel_shader_.program = program(shaders::textured_pixel);
    if (textured_vertex_shader_.program == nullptr || textured_pixel_shader_.program == nullptr) return false;
    textured_vertex_shader_.regs.sq_pgm_resources_vs = 5u | (1u << 8);
    textured_vertex_shader_.regs.sq_vtx_semantic_clear = 0xFFFFFFF8u;
    textured_vertex_shader_.regs.num_sq_vtx_semantic = 3;
    textured_vertex_shader_.regs.sq_vtx_semantic[2] = 2;
    textured_vertex_shader_.regs.spi_vs_out_id[0] = 0xFFFF0100u;
    textured_vertex_shader_.regs.spi_vs_out_config = 2;
    textured_pixel_shader_.regs.sq_pgm_resources_ps = 3;
    textured_pixel_shader_.regs.spi_ps_in_control_0 = 2u | (1u << 28);
    textured_pixel_shader_.regs.num_spi_ps_input_cntl = 2;
    textured_pixel_shader_.regs.spi_ps_input_cntls[1] = 0x101u;
    textured_pixel_shader_.samplerVarCount = 1;
    textured_pixel_shader_.samplerVars = &sampler_var_;

    const GX2AttribStream attributes[3]{
        {0, 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32, GX2_ATTRIB_INDEX_PER_VERTEX,
         0, GX2_SEL_MASK(0, 1, 2, 5), GX2_ENDIAN_SWAP_NONE},
        {1, 0, 12, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32, GX2_ATTRIB_INDEX_PER_VERTEX,
         0, GX2_SEL_MASK(0, 1, 2, 3), GX2_ENDIAN_SWAP_NONE},
        {2, 0, 28, GX2_ATTRIB_FORMAT_FLOAT_32_32, GX2_ATTRIB_INDEX_PER_VERTEX,
         0, GX2_SEL_MASK(0, 1, 4, 5), GX2_ENDIAN_SWAP_NONE},
    };
    const auto init_fetch = [&](GX2FetchShader& shader, std::uint32_t count) {
        const auto size = GX2CalcFetchShaderSizeEx(count, GX2_FETCH_SHADER_TESSELLATION_NONE,
                                                 GX2_TESSELLATION_MODE_DISCRETE);
        auto* fetch = static_cast<std::uint8_t*>(Allocate(size, GX2_SHADER_PROGRAM_ALIGNMENT));
        if (fetch == nullptr) return false;
        GX2InitFetchShaderEx(&shader, fetch, count, attributes, GX2_FETCH_SHADER_TESSELLATION_NONE,
                             GX2_TESSELLATION_MODE_DISCRETE);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, fetch, size);
        return true;
    };
    return init_fetch(fetch_shader_, 2) && init_fetch(textured_fetch_shader_, 3);
}

void GX2Manager::FreeUploads() noexcept {
    for (auto* upload : frame_uploads_) MEMFreeToDefaultHeap(upload);
    frame_uploads_.clear();
    texture0_ = {};
    texture_enabled_ = false;
}

void GX2Manager::DrawTriangles(const void* vtx_data, std::uint32_t count, std::uint32_t stride) {
    if (count == 0) return;
    if (!initialized_ || !frame_active_) throw std::logic_error("Triangle draw requires an active GX2 frame");
    const std::uint64_t size = std::uint64_t{count} * stride;
    if (vtx_data == nullptr || count % 3 != 0 || stride < 28 || stride > 2044 ||
        (stride & 3u) != 0 || (texture_enabled_ && stride < 36) || size > 128u * 1024u * 1024u) {
        throw std::invalid_argument("Invalid GX2 MVP vertex buffer");
    }
    auto* upload = Allocate(static_cast<std::uint32_t>(size), GX2_VERTEX_BUFFER_ALIGNMENT);
    if (upload == nullptr) throw std::bad_alloc{};
    try { frame_uploads_.push_back(upload); }
    catch (...) { MEMFreeToDefaultHeap(upload); throw; }
    // Raw guest float bytes are LE already; fetch streams use NO endian swap.
    std::memcpy(upload, vtx_data, static_cast<std::size_t>(size));
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, upload, static_cast<std::uint32_t>(size));
    for (const auto output : {Output::TV, Output::DRC}) {
        BindOutput(output);
        GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_REGISTER);
        GX2SetFetchShader(texture_enabled_ ? &textured_fetch_shader_ : &fetch_shader_);
        GX2SetVertexShader(texture_enabled_ ? &textured_vertex_shader_ : &vertex_shader_);
        GX2SetPixelShader(texture_enabled_ ? &textured_pixel_shader_ : &pixel_shader_);
        if (texture_enabled_) {
            GX2SetPixelTexture(&texture0_, 0);
            GX2SetPixelSampler(&sampler0_, 0);
        }
        // Each output has a separate GX2 context; restore constants after binding it.
        GX2SetVertexUniformReg(0, static_cast<std::uint32_t>(vertex_uniforms_.size()), vertex_uniforms_.data());
        GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
        GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
        GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, FALSE, TRUE);
        const auto none = static_cast<GX2ChannelMask>(0);
        GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, none, none, none, none, none, none, none);
        GX2SetAttribBuffer(0, static_cast<std::uint32_t>(size), stride, upload);
        GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, count, 0, 1);
    }
    BindOutput(Output::TV);
}

void GX2Manager::SetVertexUniforms(const float* data, std::uint32_t count) {
    if (data == nullptr || count == 0 || count > vertex_uniforms_.size() || (count & 3u) != 0)
        throw std::invalid_argument("Invalid vertex uniform scalar count");
    if (!initialized_ || !frame_active_)
        throw std::logic_error("Vertex uniform update requires an active GX2 frame");
    std::copy_n(data, count, vertex_uniforms_.begin());
    // Native float values: GX2 emits PPC numeric constants into the command stream.
    GX2SetVertexUniformReg(0, count, vertex_uniforms_.data());
}

void GX2Manager::BindTexture0(const std::uint8_t* pixels, std::uint32_t width,
                              std::uint32_t height, GX2SurfaceFormat format) {
    if (!initialized_ || !frame_active_) throw std::logic_error("Texture binding requires an active GX2 frame");
    if (pixels == nullptr || width == 0 || height == 0 || width > 1024 || height > 1024 ||
        format != GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8)
        throw std::invalid_argument("Texture upload requires linear RGBA8, maximum 1024x1024");
    GX2Texture texture{};
    auto& surface = texture.surface;
    surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    surface.width = width; surface.height = height; surface.depth = 1;
    surface.mipLevels = 1; surface.format = format; surface.aa = GX2_AA_MODE1X;
    surface.use = GX2_SURFACE_USE_TEXTURE;
    surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    GX2CalcSurfaceSizeAndAlignment(&surface);
    const auto used_bytes = std::uint64_t{surface.pitch} * (height - 1u) * 4u + std::uint64_t{width} * 4u;
    if (surface.pitch < width || used_bytes > surface.imageSize)
        throw std::runtime_error("GX2 texture surface cannot hold the linear rows");
    surface.image = Allocate(surface.imageSize, surface.alignment);
    if (surface.image == nullptr) throw std::bad_alloc{};
    try { frame_uploads_.push_back(surface.image); }
    catch (...) { MEMFreeToDefaultHeap(surface.image); throw; }
    auto* destination = static_cast<std::uint8_t*>(surface.image);
    for (std::uint32_t y = 0; y < height; ++y)
        std::memcpy(destination + static_cast<std::size_t>(y) * surface.pitch * 4u,
                    pixels + static_cast<std::size_t>(y) * width * 4u, static_cast<std::size_t>(width) * 4u);
    texture.viewNumMips = texture.viewNumSlices = 1;
    texture.compMap = GX2_COMP_MAP(0, 1, 2, 3);
    GX2InitTextureRegs(&texture);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, surface.image, surface.imageSize);
    GX2InitSampler(&sampler0_, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
    GX2InitSamplerLOD(&sampler0_, 0.0f, 0.0f, 0.0f);
    texture0_ = texture;
    texture_enabled_ = true;
    GX2SetPixelTexture(&texture0_, 0);
    GX2SetPixelSampler(&sampler0_, 0);
}

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

    if (!AllocateTarget(tv_, Output::TV) || !AllocateTarget(drc_, Output::DRC) || !InitializeShaders()) {
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
    FreeUploads();
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
    FreeUploads();
    if (fetch_shader_.program != nullptr) MEMFreeToDefaultHeap(fetch_shader_.program);
    if (vertex_shader_.program != nullptr) MEMFreeToDefaultHeap(vertex_shader_.program);
    if (pixel_shader_.program != nullptr) MEMFreeToDefaultHeap(pixel_shader_.program);
    if (textured_fetch_shader_.program != nullptr) MEMFreeToDefaultHeap(textured_fetch_shader_.program);
    if (textured_vertex_shader_.program != nullptr) MEMFreeToDefaultHeap(textured_vertex_shader_.program);
    if (textured_pixel_shader_.program != nullptr) MEMFreeToDefaultHeap(textured_pixel_shader_.program);
    fetch_shader_ = {};
    vertex_shader_ = {};
    pixel_shader_ = {};
    textured_fetch_shader_ = {};
    textured_vertex_shader_ = {};
    textured_pixel_shader_ = {};
    gx2_started_ = initialized_ = frame_active_ = presented_ = false;
}

} // namespace wiiu::gx2
