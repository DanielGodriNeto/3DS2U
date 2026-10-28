#include "pica200.hpp"
#include "gx2_manager.hpp"
#include "wiiu_memory.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value) { if (!value) throw std::runtime_error("PICA geometry regression"); }
}

int main() {
    wiiu::gx2::GX2Manager graphics;
    wiiu::Pica200 pica(graphics);
    wiiu::WiiUMemory memory(pica);
    std::array<std::uint8_t, 84> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i);
    memory.LoadImage(0x08001000u, bytes);
    memory.Write32(0x1EF00200u, 0x08001000u);
    memory.Write32(0x1EF00204u, 28);
    memory.Write32(0x1EF00208u, 3);
    memory.Write32(0x1EF0020Cu, 1);
    Check(graphics.draws == 1 && graphics.vertex_count == 3 && graphics.vertex_stride == 28);
    Check(std::equal(bytes.begin(), bytes.end(), graphics.vertex_bytes.begin()));
    memory.Write32(0x1EF0020Cu, 2);
    Check(graphics.draws == 1);

    const auto reject = [&](std::uint32_t address, std::uint32_t stride, std::uint32_t count) {
        memory.Write32(0x1EF00200u, address);
        memory.Write32(0x1EF00204u, stride);
        memory.Write32(0x1EF00208u, count);
        bool caught = false;
        try { memory.Write32(0x1EF0020Cu, 1); }
        catch (const std::exception&) { caught = true; }
        Check(caught && graphics.draws == 1);
    };
    reject(0x08001001u, 28, 3);
    reject(0x08001000u, 16, 3);
    reject(0x08001000u, 2048, 3);
    reject(0x08001000u, 28, 4);
    reject(0x0800FFF0u, 28, 3);
    reject(0x08001000u, 28, 0xFFFFFFFFu);
    memory.Write32(0x1EF00208u, 0);
    memory.Write32(0x1EF0020Cu, 1);
    Check(graphics.draws == 1);
    std::cout << "PICA geometry payload, trigger, bounds and overflow checks passed\n";
}
