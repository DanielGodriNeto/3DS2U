#include <coreinit/thread.h>
#include <coreinit/debug.h>
#include <whb/proc.h>
#include <whb/log.h>
#include <proc_ui/procui.h>
#include <vpad/input.h>

#include "wiiu_memory.hpp"
#include "wiiu_engine.hpp"
#include "hle_services.hpp"
#include "hle_hid.hpp"
#include "gx2_manager.hpp"
#include "pica200.hpp"

#include <cstdint>
#include <exception>
#include <stdexcept>

// Defined by the translated source linked into this RPX.
void InitDispatcher(wiiu::WiiUEngine& engine);

namespace {

void ConsoleLog(const char* message) {
    OSReport("%s\n", message);
}

std::uint32_t ReleaseGraphics(void* context) {
    // Stop this prototype before ProcUI releases the graphics foreground.
    static_cast<wiiu::gx2::GX2Manager*>(context)->Shutdown();
    WHBProcStopRunning();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    WHBProcInit();
    const bool log_registered = WHBAddLogHandler(ConsoleLog) != FALSE;

    wiiu::gx2::GX2Manager gx2;
    wiiu::hle::HLEServiceManager services;
    wiiu::hle::HLEHID hid;
    bool srv_initialized = false;
    bool apt_initialized = false;
    bool vpad_initialized = false;
    int result = 0;

    try {
        if (!gx2.Initialize()) {
            throw std::runtime_error("GX2 initialization failed");
        }
        ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, ReleaseGraphics, &gx2, 100);
        VPADInit();
        vpad_initialized = true;

        // The bridge outlives the engine's memory and references the active GX2 owner.
        wiiu::Pica200 pica200(gx2);
        // The engine owns CPUContext and the sole 128 MiB WiiUMemory instance.
        wiiu::WiiUEngine engine;
        engine.GetMemory().SetPica200(pica200);
        auto& cpu = engine.GetCPU();
        InitDispatcher(engine);

        if (services.srvInit() != 0) {
            throw std::runtime_error("srvInit failed");
        }
        srv_initialized = true;
        if (services.aptInit() != 0) {
            throw std::runtime_error("aptInit failed");
        }
        apt_initialized = true;
        cpu.r[15] = 0x08000000u;

        while (WHBProcIsRunning() && services.aptMainLoop()) {
            hid.ScanInput();
            gx2.BeginFrame();
            engine.Run(cpu.r[15]);
            if (engine.IsHalted()) {
                result = 1;
                break;
            }
            gx2.EndFrame();
        }
    } catch (const std::exception& error) {
        WHBLogPrintf("[3DS2U] Runtime stopped: %s", error.what());
        result = 1;
    }

    if (apt_initialized) {
        (void)services.aptExit();
    }
    if (srv_initialized) {
        (void)services.srvExit();
    }
    if (vpad_initialized) {
        VPADShutdown();
    }
    gx2.Shutdown();
    if (log_registered) {
        (void)WHBRemoveLogHandler(ConsoleLog);
    }
    WHBProcShutdown();
    return result;
}
