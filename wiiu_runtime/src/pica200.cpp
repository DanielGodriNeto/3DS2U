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
    return memory_->GetPointer(request.address, static_cast<std::size_t>(bytes));
}

void Pica200::SubmitDraw(const DrawRequest& request) {
    const auto* vertices = ValidateDraw(request);
    gx2_manager_.SetVertexUniforms(request.uniforms.data(), kUniformFloatCount);
    gx2_manager_.DrawTriangles(vertices, request.count, request.stride);
}

void Pica200::WriteMMIO(std::uint32_t address, std::uint32_t value, std::uint8_t byte_mask) {
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
    case kDrawRegister: {
        if ((byte_mask & 1u) == 0) return;
        if (value != 1 || vertex_count_ == 0) return;
        const DrawRequest request{vbo_addr_, vbo_stride_, vertex_count_, GetVertexUniforms()};
        if (batching_) pending_draws_.push_back(request);
        else SubmitDraw(request);
        break;
    }
    default: break; // Retained register state; shader/uniform interpretation is separate.
    }
}

} // namespace wiiu
