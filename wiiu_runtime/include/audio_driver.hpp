#pragma once

#include <array>
#include <cstdint>
#include <memory>

namespace wiiu {
class WiiUMemory;

// Exclusive AX owner. Serialize submission, MMIO and Pump on the runtime thread.
// AX reads owned DMA snapshots, never a guest buffer that can change underneath it.
class AudioDriver {
public:
    static constexpr std::uint32_t kVoiceCount = 16;
    static constexpr std::uint32_t kChannelBase = 0x1EC10100u;
    static constexpr std::uint32_t kChannelStride = 0x20u;
    struct ADPCMParameters {
        std::array<std::int16_t, 16> coefficients{};
        std::array<std::int16_t, 2> history{};
        std::array<std::int16_t, 2> loop_history{};
    };

    explicit AudioDriver(WiiUMemory& memory);
    ~AudioDriver() noexcept;
    AudioDriver(const AudioDriver&) = delete;
    AudioDriver& operator=(const AudioDriver&) = delete;
    [[nodiscard]] bool Initialize();
    void Shutdown() noexcept;
    // Project format IDs: 0 unsigned PCM8, 1 signed LE PCM16, 2 Nintendo DSP ADPCM.
    // Mono, complete buffer; ADPCM requires coefficients/history configured first.
    void PlayVoice(std::uint32_t channel, std::uint32_t fcram_addr, std::uint32_t sample_rate,
                   std::uint16_t format, std::uint32_t size_bytes, bool loop);
    void SetADPCMParameters(std::uint32_t channel, const ADPCMParameters& parameters);
    void SetVolume(std::uint32_t channel, float volume);
    void SetPitch(std::uint32_t channel, float multiplier);
    void StopVoice(std::uint32_t channel);
    void StopAll() noexcept;
    void Pump() noexcept;
    [[nodiscard]] bool IsPlaying(std::uint32_t channel) const;
    // Synthetic MVP registers, separate from real CSND shared-memory commands.
    bool WriteMMIO(std::uint32_t address, std::uint32_t value, unsigned width);
    [[nodiscard]] bool ReadMMIO(std::uint32_t address, unsigned width, std::uint32_t& value) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace wiiu
