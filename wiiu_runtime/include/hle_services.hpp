#pragma once

#include <cstdint>

namespace wiiu::hle {

// Placeholder 3DS srv:/apt: responses; service handles and IPC are not emulated.
class HLEServiceManager {
public:
    [[nodiscard]] std::uint32_t srvInit();
    [[nodiscard]] std::uint32_t aptInit();
    [[nodiscard]] std::uint32_t aptMainLoop();
    [[nodiscard]] std::uint32_t aptExit();
    [[nodiscard]] std::uint32_t srvExit();
};

} // namespace wiiu::hle
