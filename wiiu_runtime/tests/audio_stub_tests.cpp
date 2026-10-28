#include "hle_services.hpp"
#include "wiiu_memory.hpp"
#include "pica200.hpp"

#include <vpad/input.h>
#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::vector<std::string> logs;
void Check(bool condition) {
    if (!condition) throw std::runtime_error("Audio stub test failed");
}
template<class Function> void CheckRejected(Function operation) {
    bool rejected = false;
    try { operation(); }
    catch (const std::out_of_range&) { rejected = true; }
    Check(rejected);
}
} // namespace

// Replace only platform allocation/logging and unrelated input/graphics APIs.
// The suite links the production service manager and WiiUMemory implementations.
void* MEMAllocFromDefaultHeapEx(std::uint32_t size, std::int32_t alignment) {
    Check(alignment == 0x1000);
    return ::operator new(size, std::align_val_t{0x1000}, std::nothrow);
}
void MEMFreeToDefaultHeap(void* pointer) {
    ::operator delete(pointer, std::align_val_t{0x1000});
}
void OSReport(const char* format, ...) {
    char message[512]{};
    va_list arguments;
    va_start(arguments, format);
    const auto length = std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    Check(length >= 0 && static_cast<std::size_t>(length) < sizeof(message));
    logs.emplace_back(message);
}
void wiiu::Pica200::WriteMMIO(std::uint32_t, std::uint32_t) {
    throw std::logic_error("Audio access reached graphics bridge");
}
std::int32_t VPADRead(VPADChan, VPADStatus*, std::uint32_t, VPADReadError* error) {
    *error = VPAD_READ_NO_SAMPLES;
    return 0;
}
void VPADGetTPCalibratedPointEx(VPADChan, VPADTouchPadResolution,
                               VPADTouchData*, const VPADTouchData*) {}

int main() {
    using Services = wiiu::hle::HLEServiceManager;
    Services services;
    Check(services.csndInit() == 0);
    Check(services.dspInit() == 0);
    Check(services.dspExit() == 0);
    Check(services.csndExit() == 0);
    Check(logs == std::vector<std::string>{
        "[OSConsole] [3DS2U HLE] csndInit: stub success\n",
        "[OSConsole] [3DS2U HLE] dspInit: stub success\n",
        "[OSConsole] [3DS2U HLE] dspExit: stub success\n",
        "[OSConsole] [3DS2U HLE] csndExit: stub success\n"});
    for (std::uint32_t channel = 0; channel < 32; ++channel) {
        Check(services.dspGetSemaphoreState() == 1);
        Check(services.dspIsChannelReady(channel));
        Check(!services.csndIsChannelPlaying(channel));
    }

    wiiu::WiiUMemory memory;
    constexpr auto base = Services::kAudioMMIOBase;
    constexpr auto end = Services::kAudioMMIOEnd;
    constexpr auto ram = wiiu::WiiUMemory::kVirtualBase;
    memory.Write32(ram, 0x12345678);
    memory.Write16(ram + 4, 0xABCD);
    memory.Write32(base, 0xDEADBEEF);
    memory.Write16(base, 0xFFFF);
    Check(memory.Read32(base) == 0 && memory.Read16(base) == 0);
    Check(logs.back().find("Write16 0x1EC10000 = 0x0000FFFF: stub no-op") != std::string::npos);
    for (auto address = base; address < end; address += 4) {
        const auto expected = (address == Services::kAudioReadyRegister ||
                               address == Services::kAudioSemaphoreRegister) ? 1u : 0u;
        Check(memory.Read32(address) == expected);
        Check(memory.Read16(address) == expected);
        Check(memory.Read16(address + 2) == 0);
    }
    memory.Write32(Services::kAudioReadyRegister, 0);
    memory.Write16(Services::kAudioSemaphoreRegister, 0);
    Check(memory.Read32(Services::kAudioReadyRegister) == 1);
    Check(memory.Read32(Services::kAudioSemaphoreRegister) == 1);
    memory.Write32(end - 4, 0xFFFFFFFF);
    memory.Write16(end - 2, 0xFFFF);
    Check(memory.Read32(end - 4) == 0 && memory.Read16(end - 2) == 0);
    Check(memory.Read32(ram) == 0x12345678 && memory.Read16(ram + 4) == 0xABCD);
    const auto* bytes = static_cast<const unsigned char*>(memory.GetPointer(ram, 6));
    Check(bytes[0] == 0x78 && bytes[3] == 0x12 && bytes[4] == 0xCD && bytes[5] == 0xAB);
    for (auto address : {base - 4, base + 1, end - 2, end}) {
        CheckRejected([&] { (void)memory.Read32(address); });
        CheckRejected([&] { memory.Write32(address, 0); });
    }
    CheckRejected([&] { (void)memory.Read16(base + 1); });
    CheckRejected([&] { memory.Write16(end - 1, 0); });
    CheckRejected([&] { (void)Services::ReadAudioMMIO(base, 1); });
    std::cout << "Audio services, production MMIO dispatch, boundaries and LE memory tests passed.\n";
}
