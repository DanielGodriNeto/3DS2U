#include "hle_services.hpp"

#include <coreinit/debug.h>

#include <cinttypes>
#include <stdexcept>

namespace wiiu::hle {
namespace {
void ValidateAudioAccess(std::uint32_t address, unsigned width) {
    if ((width != 2 && width != 4) || address < HLEServiceManager::kAudioMMIOBase ||
        address >= HLEServiceManager::kAudioMMIOEnd ||
        width > HLEServiceManager::kAudioMMIOEnd - address ||
        (address & (width - 1u)) != 0) {
        throw std::out_of_range("Invalid audio MMIO access");
    }
}
} // namespace

std::uint32_t HLEServiceManager::csndInit() {
    OSReport("[OSConsole] [3DS2U HLE] csndInit: stub success\n");
    return 0;
}

std::uint32_t HLEServiceManager::csndExit() {
    OSReport("[OSConsole] [3DS2U HLE] csndExit: stub success\n");
    return 0;
}

std::uint32_t HLEServiceManager::dspInit() {
    OSReport("[OSConsole] [3DS2U HLE] dspInit: stub success\n");
    return 0;
}

std::uint32_t HLEServiceManager::dspExit() {
    OSReport("[OSConsole] [3DS2U HLE] dspExit: stub success\n");
    return 0;
}

std::uint32_t HLEServiceManager::ReadAudioMMIO(std::uint32_t address, unsigned width) {
    ValidateAudioAccess(address, width);
    const auto word_address = address & ~3u;
    const std::uint32_t value =
        (word_address == kAudioReadyRegister || word_address == kAudioSemaphoreRegister) ? 1u : 0u;
    // MMIO returns a numeric guest value; select halfwords in guest LE order.
    return (value >> ((address & 3u) * 8u)) & (width == 2 ? 0xFFFFu : 0xFFFFFFFFu);
}

void HLEServiceManager::WriteAudioMMIO(std::uint32_t address, std::uint32_t value, unsigned width) {
    ValidateAudioAccess(address, width);
    OSReport("[OSConsole] [3DS2U HLE] audio MMIO Write%u 0x%08" PRIX32
             " = 0x%08" PRIX32 ": stub no-op\n", width * 8u, address, value);
}

std::uint32_t HLEServiceManager::srvInit() {
    OSReport("[3DS2U HLE] srvInit: stub success\n");
    return 0;
}

std::uint32_t HLEServiceManager::aptInit() {
    OSReport("[3DS2U HLE] aptInit: stub success\n");
    return 0;
}

std::uint32_t HLEServiceManager::aptMainLoop() {
    OSReport("[3DS2U HLE] aptMainLoop: stub continue\n");
    return 1;
}

std::uint32_t HLEServiceManager::aptExit() {
    OSReport("[3DS2U HLE] aptExit: stub success\n");
    return 0;
}

std::uint32_t HLEServiceManager::srvExit() {
    OSReport("[3DS2U HLE] srvExit: stub success\n");
    return 0;
}

} // namespace wiiu::hle
