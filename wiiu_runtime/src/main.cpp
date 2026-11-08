#include <coreinit/thread.h>
#include <coreinit/debug.h>
#include <whb/proc.h>
#include <whb/log.h>
#include <proc_ui/procui.h>
#include <vpad/input.h>

#include "wiiu_memory.hpp"
#include "wiiu_engine.hpp"
#include "hle_services.hpp"
#include "gx2_manager.hpp"
#include "pica200.hpp"
#include "display_transfer.hpp"
#include "gsp_gpu.hpp"
#include "audio_driver.hpp"
#include "packaged_romfs.hpp"

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string_view>

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

#ifdef DS2U_MISSING_BLOCK_DETAIL
// Diagnostics-only lookup: context is the CRO loader that owns the loaded modules.
const char* LookupCroModule(void* context, std::uint32_t address) noexcept {
    return static_cast<wiiu::CROLoader*>(context)->FindModuleName(address);
}
#endif

} // namespace

int main(int argc, char** argv) {
    bool is_new_3ds = false;
    for (int i = 1; i < argc; ++i)
        if (argv[i] && std::string_view(argv[i]) == "--new-3ds") is_new_3ds = true;
    WHBProcInit();
    if (!OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU1))
        OSReport("[3DS2U] Could not pin rendering thread to Core 1\n");
    const bool log_registered = WHBAddLogHandler(ConsoleLog) != FALSE;

    wiiu::gx2::GX2Manager gx2;
    wiiu::hle::HLEServiceManager services;
    wiiu::RomFSReader romfs;
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
        wiiu::DisplayTransfer display(gx2);
        wiiu::GSPGPU gsp_gpu(pica200, display);
        services.AttachGSPGPU(gsp_gpu);
        // The engine owns the selected O3DS/N3DS memory and input model.
        wiiu::WiiUEngine engine(is_new_3ds);
        // Engine shutdown joins guest workers before releasing AX and its DMA snapshots.
        auto& audio = engine.GetAudioDriver();
        if (!audio.Initialize()) throw std::runtime_error("AX initialization failed");
        services.AttachAudioDriver(audio);
        engine.GetMemory().SetPica200(pica200);
        auto& cpu = engine.GetCPU();
        InitDispatcher(engine);
#ifdef DS2U_MISSING_BLOCK_DETAIL
        engine.EnableMissingBlockDiagnostics(true);
        engine.SetModuleLookup(LookupCroModule, &services.GetCROLoader());
#endif
        if (const auto assets=wiiu::GetPackagedRomFS(); !assets.empty()) {
            if (!romfs.Mount(assets)) throw std::runtime_error("Packaged RomFS mount failed: "+romfs.GetLastError());
            services.AttachRomFS(romfs);
            if (services.FSUSER_Initialize()!=0) throw std::runtime_error("Packaged RomFS service initialization failed");
            WHBLogPrintf("[3DS2U] Mounted packaged RomFS: %u bytes",static_cast<unsigned>(assets.size()));
        }

        if (services.srvInit() != 0) {
            throw std::runtime_error("srvInit failed");
        }
        srv_initialized = true;
        if (services.aptInit() != 0) {
            throw std::runtime_error("aptInit failed");
        }
        apt_initialized = true;

        while (WHBProcIsRunning() && services.aptMainLoop()) {
            gx2.BeginFrame();
            engine.Run(cpu.r[15]);
            if (cpu.svc_exit_requested) break;
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
