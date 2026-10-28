#include "pica200.hpp"
#include "gx2_manager.hpp"

namespace wiiu {

void Pica200::WriteMMIO(std::uint32_t address, std::uint32_t value) {
    if (address == kClearColorRegister) {
        const float r = static_cast<float>((value >> 16) & 0xFFu) / 255.0f;
        const float g = static_cast<float>((value >> 8) & 0xFFu) / 255.0f;
        const float b = static_cast<float>(value & 0xFFu) / 255.0f;
        gx2_manager_.SetClearColor(r, g, b, 1.0f);
    }
    // Other MMIO writes are ignored until their PICA200 commands are modeled.
}

} // namespace wiiu
