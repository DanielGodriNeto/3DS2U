// Native AX playback test; no guest firmware or translated ARM code is executed.
#include "audio_driver.hpp"
#include "hle_services.hpp"
#include "wiiu_memory.hpp"
#include <coreinit/debug.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/proc.h>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <vector>

int main() {
    WHBProcInit();
    int result = 0;
    try {
        wiiu::WiiUMemory memory;
        wiiu::AudioDriver audio(memory);
        wiiu::hle::HLEServiceManager services;
        services.AttachAudioDriver(audio);
        if (services.csndInit() != 0) throw std::runtime_error("AX initialization failed");
        memory.SetAudioDriver(&audio);
        constexpr std::uint32_t rate = 32000, samples = 8000, address = 0x08001000u;
        std::vector<std::uint8_t> pcm(samples * 2u);
        for (std::uint32_t i = 0; i < samples; ++i) {
            const auto value = static_cast<std::int16_t>(std::lround(
                std::sin(6.283185307179586 * 440.0 * static_cast<double>(i) / rate) * 4096.0));
            const auto bits = static_cast<std::uint16_t>(value);
            pcm[i * 2] = static_cast<std::uint8_t>(bits);
            pcm[i * 2 + 1] = static_cast<std::uint8_t>(bits >> 8);
        }
        memory.LoadImage(address, pcm);
        audio.SetVolume(0, 0.5f);
        if (services.csndPlaySound(0,address,rate,1,static_cast<std::uint32_t>(pcm.size()),true) != 0)
            throw std::runtime_error("Sine-wave submission failed");
        OSReport("[3DS2U Audio] Looping 440 Hz PCM16 tone on TV and DRC\n");
        while (WHBProcIsRunning()) {
            audio.Pump(); OSSleepTicks(OSMillisecondsToTicks(10));
        }
        (void)services.csndExit();
        memory.SetAudioDriver(nullptr);
    } catch (const std::exception& error) {
        OSReport("[3DS2U Audio] %s\n",error.what()); result = 1;
    }
    WHBProcShutdown(); return result;
}
