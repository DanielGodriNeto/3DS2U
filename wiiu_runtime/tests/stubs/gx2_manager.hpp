#pragma once

#include <array>

namespace wiiu::gx2 {

// Host test double; production builds use wiiu_runtime/include/gx2_manager.hpp.
class GX2Manager {
public:
    void SetClearColor(float r, float g, float b, float a) noexcept {
        color = {r, g, b, a};
        ++writes;
    }

    std::array<float, 4> color{};
    unsigned writes{};
};

} // namespace wiiu::gx2
