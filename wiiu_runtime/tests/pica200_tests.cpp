#include "pica200.hpp"
#include "gx2_manager.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value) {
    if (!value) throw std::runtime_error("PICA200 clear-color regression");
}
}

int main() {
    wiiu::gx2::GX2Manager graphics;
    wiiu::Pica200 pica(graphics);
    unsigned expected_writes = 0;
    for (unsigned channel = 0; channel < 3; ++channel) {
        for (std::uint32_t intensity = 0; intensity < 256; ++intensity) {
            // The upper byte must not influence the RGB register or GX2 alpha.
            const auto value = 0xAB000000u | (intensity << ((2u - channel) * 8u));
            pica.WriteMMIO(0x1EF00000u, value);
            Check(graphics.writes == ++expected_writes);
            for (unsigned component = 0; component < 3; ++component) {
                const float expected = component == channel ?
                    static_cast<float>(intensity) / 255.0f : 0.0f;
                Check(std::abs(graphics.color[component] - expected) < 0.000001f);
            }
            Check(graphics.color[3] == 1.0f);
        }
    }
    pica.WriteMMIO(0x1EF00000u, 0x00123456u);
    Check(graphics.writes == ++expected_writes);
    Check(std::abs(graphics.color[0] - 18.0f / 255.0f) < 0.000001f);
    Check(std::abs(graphics.color[1] - 52.0f / 255.0f) < 0.000001f);
    Check(std::abs(graphics.color[2] - 86.0f / 255.0f) < 0.000001f);
    const auto before = graphics.color;
    for (const auto address : {0x1EEFFFFFu, 0x1EF00004u, 0x1EFFFFFEu,
                               0x1EFFFFFFu, 0x1F000000u}) {
        pica.WriteMMIO(address, 0xFFFFFFFFu);
        Check(graphics.writes == expected_writes && graphics.color == before);
    }
    std::cout << "PICA200 RGB conversion passed (769 colors and ignored-register checks)\n";
}
