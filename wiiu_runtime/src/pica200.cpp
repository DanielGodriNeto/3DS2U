#include "pica200.hpp"
#include "gx2_manager.hpp"
#include "wiiu_memory.hpp"
#include <limits>
#include <stdexcept>

namespace wiiu {

void Pica200::WriteMMIO(std::uint32_t address, std::uint32_t value) {
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
        if (value != 1 || vertex_count_ == 0) return;
        if (memory_ == nullptr) throw std::logic_error("PICA200 vertex memory is not attached");
        if ((vbo_addr_ & 3u) != 0 || vbo_stride_ < 28 || vbo_stride_ > 2044 ||
            (vbo_stride_ & 3u) != 0 || vertex_count_ % 3 != 0) {
            throw std::invalid_argument("Invalid MVP triangle buffer layout/count");
        }
        const std::uint64_t bytes = std::uint64_t{vertex_count_} * vbo_stride_;
        if (bytes > std::numeric_limits<std::uint32_t>::max()) {
            throw std::out_of_range("PICA200 vertex buffer size overflows");
        }
        const auto* vertices = memory_->GetPointer(vbo_addr_, static_cast<std::size_t>(bytes));
        gx2_manager_.DrawTriangles(vertices, vertex_count_, vbo_stride_);
        break;
    }
    default: break; // Other PICA200 registers are outside this MVP protocol.
    }
}

} // namespace wiiu
