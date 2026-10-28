#include "pica200.hpp"
#include "gx2_manager.hpp"
#include "wiiu_memory.hpp"
#include "float24.hpp"
#include <limits>
#include <stdexcept>

namespace wiiu {

std::uint32_t Pica200::GetRegisterValue(std::uint32_t address) const noexcept {
    const auto it = registers_.find(address);
    if (it != registers_.end()) return it->second;
    if (address >= kUniformBase && address < kUniformEnd && (address & 3u) == 0)
        return uniforms24_[(address - kUniformBase) / 4u];
    return 0;
}

Pica200::VertexUniforms Pica200::GetVertexUniforms() const noexcept {
    VertexUniforms result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = float24_to_float32(uniforms24_[i]);
    return result;
}

void Pica200::BeginCommandList(WiiUMemory& memory) {
    if (batching_) throw std::logic_error("Nested PICA command lists are unsupported");
    AttachMemory(memory);
    pending_draws_.clear();
    batching_ = true;
}

void Pica200::CancelCommandList() noexcept {
    pending_draws_.clear();
    batching_ = false;
}

bool Pica200::EndCommandList() {
    if (!batching_) throw std::logic_error("No active PICA command list");
    for (const auto& request : pending_draws_) (void)ValidateDraw(request);
    for (const auto& request : pending_draws_) SubmitDraw(request);
    const bool drew = !pending_draws_.empty();
    CancelCommandList();
    return drew;
}

const void* Pica200::ValidateDraw(const DrawRequest& request) const {
    if (memory_ == nullptr) throw std::logic_error("PICA200 vertex memory is not attached");
    if ((request.address & 3u) != 0 || request.stride < 28 || request.stride > 2044 ||
        (request.stride & 3u) != 0 || request.count % 3 != 0) {
        throw std::invalid_argument("Invalid MVP triangle buffer layout/count");
    }
    const std::uint64_t bytes = std::uint64_t{request.count} * request.stride;
    if (bytes > std::numeric_limits<std::uint32_t>::max())
        throw std::out_of_range("PICA200 vertex buffer size overflows");
    if (request.texture.address != 0) {
        if (request.stride < 36) throw std::invalid_argument("Textured vertices require float2 UV at offset 28");
        const auto size = TextureSourceSize(request.texture.dimensions & 0xFFFFu,
                                            request.texture.dimensions >> 16,
                                            static_cast<TextureFormat>(request.texture.format));
        (void)memory_->GetPointer(request.texture.address, size);
    }
    return memory_->GetPointer(request.address, static_cast<std::size_t>(bytes));
}

void Pica200::SubmitDraw(const DrawRequest& request) {
    const auto* vertices = ValidateDraw(request);
    if (request.texture.address != 0) {
        const auto width = request.texture.dimensions & 0xFFFFu;
        const auto height = request.texture.dimensions >> 16;
        const auto format = static_cast<TextureFormat>(request.texture.format);
        const auto source_size = TextureSourceSize(width, height, format);
        const auto* source = static_cast<const std::uint8_t*>(memory_->GetPointer(request.texture.address, source_size));
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4u);
        Deswizzle3DSTexture(std::span{source, source_size}, std::span{rgba}, width, height, format);
        gx2_manager_.BindTexture0(rgba.data(), width, height, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8);
    } else gx2_manager_.DisableTexture0();
    gx2_manager_.SetVertexUniforms(request.uniforms.data(), kUniformFloatCount);
    gx2_manager_.DrawTriangles(vertices, request.count, request.stride);
}

void Pica200::WriteMMIO(std::uint32_t address, std::uint32_t value, std::uint8_t byte_mask) {
    // Audio is not a PICA register: direct bridge callers forward to memory's audio owner.
    if (address >= 0x1EC10000u && address < 0x1EC18000u) {
        if (memory_ == nullptr || (address & 3u) != 0 || byte_mask > 15 || batching_)
            throw std::invalid_argument("Invalid direct audio bridge write");
        if (byte_mask == 0) return;
        std::uint32_t mask{};
        for (unsigned i = 0; i < 4; ++i) if ((byte_mask & (1u << i)) != 0) mask |= 0xFFu << (i * 8);
        if (byte_mask != 15) value = (memory_->Read32(address) & ~mask) | (value & mask);
        memory_->Write32(address, value); return;
    }
    if (address < kMMIOBase || address >= kMMIOEnd || (address & 3u) != 0)
        return; // Preserve the direct-MMIO bridge's ignored-address behavior.
    if ((byte_mask & 0xFu) == 0) return;
    std::uint32_t bits = 0;
    for (unsigned byte = 0; byte < 4; ++byte)
        if ((byte_mask & (1u << byte)) != 0) bits |= 0xFFu << (byte * 8u);
    value = (GetRegisterValue(address) & ~bits) | (value & bits);
    // Shadow unknown/uniform registers too, so masked and consecutive writes persist.
    registers_[address] = value;
    if (address >= kUniformBase && address < kUniformEnd) {
        uniforms24_[(address - kUniformBase) / 4u] = value & 0xFFFFFFu;
        return;
    }
    switch (address) {
    case kClearColorRegister: {
        const float r = static_cast<float>((value >> 16) & 0xFFu) / 255.0f;
        const float g = static_cast<float>((value >> 8) & 0xFFu) / 255.0f;
        const float b = static_cast<float>(value & 0xFFu) / 255.0f;
        gx2_manager_.SetClearColor(r, g, b, 1.0f);
        break;
    }
    case kVertexBufferRegister: vbo_addr_ = value; break;
    case kVertexStrideRegister: vbo_stride_ = value; break;
    case kVertexCountRegister: vertex_count_ = value; break;
    case kTextureAddress: texture0_.address = value; break;
    case kTextureDimensions: texture0_.dimensions = value; break;
    case kTextureFormat: texture0_.format = value; break;
    case kDrawRegister: {
        if ((byte_mask & 1u) == 0) return;
        if (value != 1 || vertex_count_ == 0) return;
        const DrawRequest request{vbo_addr_, vbo_stride_, vertex_count_, GetVertexUniforms(), texture0_};
        if (batching_) pending_draws_.push_back(request);
        else SubmitDraw(request);
        break;
    }
    default: break; // Retained register state; shader/uniform interpretation is separate.
    }
}

} // namespace wiiu
