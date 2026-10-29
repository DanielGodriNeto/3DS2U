#pragma once

#include <cstdint>

namespace wiiu::hle {

class HLEHID {
public:
    // GamePad Minus maps to 3DS Select; Plus maps to Start.
    void ScanInput();

    [[nodiscard]] std::uint32_t KeysHeld() const noexcept { return current_keys; }
    [[nodiscard]] std::uint32_t KeysDown() const noexcept { return down_keys; }

private:
    std::uint32_t current_keys{};
    std::uint32_t down_keys{};
};

} // namespace wiiu::hle
