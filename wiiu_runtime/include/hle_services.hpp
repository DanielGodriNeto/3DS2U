#pragma once

#include <cstdint>

namespace wiiu::hle {

// Placeholder 3DS service responses; service handles and IPC are not emulated.
class HLEServiceManager {
public:
    [[nodiscard]] std::uint32_t srvInit();
    [[nodiscard]] std::uint32_t aptInit();
    [[nodiscard]] std::uint32_t aptMainLoop();
    [[nodiscard]] std::uint32_t aptExit();
    [[nodiscard]] std::uint32_t srvExit();

    [[nodiscard]] std::uint32_t csndInit();
    [[nodiscard]] std::uint32_t csndExit();
    [[nodiscard]] std::uint32_t dspInit();
    [[nodiscard]] std::uint32_t dspExit();

    // Nonblocking HLE helpers; these are not libctru ABI/IPC implementations.
    [[nodiscard]] static std::uint32_t dspGetSemaphoreState() noexcept { return 1; }
    [[nodiscard]] static bool dspIsChannelReady(std::uint32_t /*channel*/) noexcept { return true; }
    [[nodiscard]] static bool csndIsChannelPlaying(std::uint32_t /*channel*/) noexcept { return false; }

    static constexpr std::uint32_t kAudioMMIOBase = 0x1EC10000u;
    static constexpr std::uint32_t kAudioMMIOEnd = 0x1EC18000u;
    // Synthetic MVP readiness/semaphore registers; other registers read zero.
    static constexpr std::uint32_t kAudioReadyRegister = kAudioMMIOBase + 4u;
    static constexpr std::uint32_t kAudioSemaphoreRegister = kAudioMMIOBase + 8u;
    [[nodiscard]] static std::uint32_t ReadAudioMMIO(std::uint32_t address, unsigned width);
    static void WriteAudioMMIO(std::uint32_t address, std::uint32_t value, unsigned width);
};

} // namespace wiiu::hle
