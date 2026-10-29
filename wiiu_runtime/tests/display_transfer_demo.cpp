// Standalone wut display test; this executable does not run translated ARM code.
#include "display_transfer.hpp"
#include "gsp_gpu.hpp"
#include "gx2_manager.hpp"
#include "hle_services.hpp"
#include "pica200.hpp"
#include "wiiu_memory.hpp"
#include <coreinit/debug.h>
#include <proc_ui/procui.h>
#include <whb/proc.h>
#include <array>
#include <exception>
#include <stdexcept>
#include <vector>

namespace {
std::uint32_t Release(void* owner) {
    static_cast<wiiu::gx2::GX2Manager*>(owner)->Shutdown();
    WHBProcStopRunning(); return 0;
}
void Submit(wiiu::WiiUMemory& memory, wiiu::hle::HLEServiceManager& services, bool top) {
    const std::uint32_t width = top ? 400u : 320u;
    const std::uint32_t source = top ? 0x08100000u : 0x08300000u;
    const auto destination = source + 0x100000u;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * 240u * 4u);
    for (std::uint32_t y = 0; y < 240; ++y) for (std::uint32_t x = 0; x < width; ++x) {
        const auto at = (static_cast<std::size_t>(y) * width + x) * 4u;
        const auto shade = static_cast<std::uint8_t>(((x / 20u) ^ (y / 20u)) & 1u ? 255 : 100);
        pixels[at] = 255; pixels[at + 1] = 32;
        pixels[at + 2] = top ? 32 : shade; pixels[at + 3] = top ? shade : 32;
    }
    memory.LoadImage(source, pixels);
    const auto dimensions = width | (240u << 16);
    const std::array<std::uint32_t, 8> command{
        0x01000103u, source, destination, dimensions, dimensions, 0x1022u, 0, 0};
    for (unsigned i = 0; i < command.size(); ++i) memory.Write32(0x08000000u + i * 4u, command[i]);
    if (services.GSPGPU_SubmitGXCommand(memory, 0x08000000u) != 0)
        throw std::runtime_error("Display-transfer demo submission failed");
}
} // namespace

int main() {
    WHBProcInit();
    wiiu::gx2::GX2Manager graphics;
    int result = 0;
    try {
        if (!graphics.Initialize()) throw std::runtime_error("GX2 initialization failed");
        ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, Release, &graphics, 100);
        wiiu::WiiUMemory memory;
        wiiu::Pica200 pica(graphics);
        wiiu::DisplayTransfer transfer(graphics);
        wiiu::GSPGPU gpu(pica, transfer);
        wiiu::hle::HLEServiceManager services;
        services.AttachGSPGPU(gpu);
        Submit(memory, services, true); Submit(memory, services, false);
        OSReport("[3DS2U Display] TV: red checkerboard; DRC: green checkerboard\n");
        while (WHBProcIsRunning()) { graphics.BeginFrame(); graphics.EndFrame(); }
    } catch (const std::exception& error) {
        OSReport("[3DS2U Display] %s\n", error.what()); result = 1;
    }
    graphics.Shutdown(); WHBProcShutdown(); return result;
}
