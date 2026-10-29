#include "audio_driver.hpp"
#include "hle_services.hpp"
#include "wiiu_memory.hpp"
#include "pica200.hpp"
#include <sndcore2/core.h>
#include <coreinit/cache.h>
#include <vpad/input.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <new>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using U32 = std::uint32_t;
constexpr U32 source = 0x08001000u;
std::map<void*, std::size_t> allocations;
bool fail_allocation{};
void Check(bool value, std::source_location at = std::source_location::current()) {
    if (!value) throw std::runtime_error("Audio-driver test failed at line " + std::to_string(at.line()));
}
template<class F> void Reject(F operation) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    Check(rejected);
}
} // namespace

void* MEMAllocFromDefaultHeapEx(U32 size, std::int32_t alignment) {
    if (fail_allocation) return nullptr;
    Check(alignment == 0x1000 || alignment == 0x100);
    auto* p = ::operator new(size, std::align_val_t{static_cast<std::size_t>(alignment)}, std::nothrow);
    if (p) allocations.emplace(p, static_cast<std::size_t>(alignment));
    return p;
}
void MEMFreeToDefaultHeap(void* p) {
    const auto it = allocations.find(p); Check(it != allocations.end());
    ::operator delete(p, std::align_val_t{it->second}); allocations.erase(it);
}
void OSReport(const char*, ...) {}
void wiiu::Pica200::WriteMMIO(U32, U32, std::uint8_t) { throw std::runtime_error("Unexpected graphics access"); }
std::int32_t VPADRead(VPADChan, VPADStatus*, U32, VPADReadError* error) { *error = VPAD_READ_NO_SAMPLES; return 0; }
void VPADGetTPCalibratedPointEx(VPADChan, VPADTouchPadResolution, VPADTouchData*, const VPADTouchData*) {}

int main() try {
    wiiu::WiiUMemory memory;
    wiiu::AudioDriver driver(memory);
    Reject([&] { driver.PlayVoice(0,source,48000,1,512,false); });
    Check(driver.Initialize() && ax_mock::acquired == 16);
    for (const auto& voice : ax_mock::voices)
        Check(voice.mix_calls == 2 && voice.left_mix[0] == 0x8000 && voice.right_mix[1] == 0x8000);
    wiiu::AudioDriver other(memory); Check(!other.Initialize());
    std::vector<std::uint8_t> sine(512);
    for (U32 i = 0; i < sine.size() / 2; ++i) {
        const auto sample = static_cast<std::int16_t>(std::lround(std::sin(static_cast<double>(i) * 0.2) * 16000));
        const auto bits = static_cast<std::uint16_t>(sample);
        sine[i * 2] = static_cast<std::uint8_t>(bits); sine[i * 2 + 1] = static_cast<std::uint8_t>(bits >> 8);
    }
    memory.LoadImage(source,sine); driver.PlayVoice(0,source,48000,1,static_cast<U32>(sine.size()),true);
    const auto& voice = ax_mock::voices[0];
    Check(voice.offsets.dataType == AX_VOICE_FORMAT_LPCM16 && voice.offsets.endOffset == 255);
    Check(voice.offsets.loopOffset == 0 && voice.offsets.currentOffset == 0 && voice.offsets.loopingEnabled == 1);
    Check(voice.ratio == 1.5f && voice.envelope.volume == 0x8000 && voice.src_type == AX_VOICE_SRC_TYPE_LINEAR);
    Check(voice.begins == voice.ends && driver.IsPlaying(0));
    Check((reinterpret_cast<std::uintptr_t>(voice.offsets.data) & 0xFFu) == 0);
    const auto* upload = static_cast<const std::uint8_t*>(voice.offsets.data);
    for (U32 i = 0; i < sine.size(); ++i) Check(upload[i] == sine[i ^ 1u]);
    Check(upload != memory.GetPointer(source,sine.size()));
    Check(ax_mock::stores.size() == 2 && ax_mock::stores[0].first == memory.GetPointer(source,sine.size()));
    Check(ax_mock::stores[1].first == upload && ax_mock::stores[1].second >= sine.size() + 32);
    memory.Write32(source,0); Check(upload[2] == sine[3]);
    // Failed submissions preserve the active voice and its owned buffer.
    Reject([&] { driver.PlayVoice(16,source,48000,1,512,false); });
    Reject([&] { driver.PlayVoice(0,source+1,48000,1,512,false); });
    Reject([&] { driver.PlayVoice(0,source,48000,1,511,false); });
    Reject([&] { driver.PlayVoice(0,source,0,1,512,false); });
    Reject([&] { driver.PlayVoice(0,0x10000000,48000,1,512,false); });
    Reject([&] { driver.PlayVoice(0,source,48000,3,512,false); });
    fail_allocation = true; Reject([&] { driver.PlayVoice(0,source,48000,1,512,false); }); fail_allocation = false;
    Check(voice.offsets.data == upload && driver.IsPlaying(0));
    // PCM8 changes unsigned guest samples into AX signed bytes.
    memory.LoadImage(source,std::array<std::uint8_t,3>{0,128,255});
    driver.PlayVoice(0,source,32000,0,3,false);
    const auto* pcm8 = static_cast<const std::uint8_t*>(voice.offsets.data);
    Check(voice.offsets.dataType == AX_VOICE_FORMAT_LPCM8 && voice.offsets.endOffset == 2);
    Check(pcm8[0] == 0x80 && pcm8[1] == 0 && pcm8[2] == 0x7F);
    const auto held = allocations.size(); driver.Pump(); Check(allocations.size() == held);
    ax_mock::Tick(5); driver.Pump(); Check(allocations.size() == held);
    ax_mock::Tick(1); driver.Pump(); Check(allocations.size() == held - 1);
    // Frame callbacks before the old voice update must not age its DMA allocation early.
    ax_mock::before_begin = [] { ax_mock::Tick(10); };
    driver.PlayVoice(0,source,32000,0,3,false); ax_mock::before_begin = nullptr;
    const auto preempted = allocations.size(); driver.Pump(); Check(allocations.size() == preempted);
    ax_mock::Tick(6); driver.Pump(); Check(allocations.size() == preempted - 1);
    // ADPCM requires explicit coefficients and predictor histories; offsets use nibbles.
    memory.LoadImage(source,std::array<std::uint8_t,8>{0x21,0x12,0x34,0x56,0x78,0x9A,0xBC,0xDE});
    Reject([&] { driver.PlayVoice(1,source,32000,2,8,true); });
    wiiu::AudioDriver::ADPCMParameters parameters;
    parameters.coefficients[4] = 1024; parameters.history = {10,-10}; parameters.loop_history = {20,-20};
    driver.SetADPCMParameters(1,parameters); driver.PlayVoice(1,source,32000,2,8,true);
    const auto& adpcm = ax_mock::voices[1];
    Check(adpcm.offsets.dataType == AX_VOICE_FORMAT_ADPCM && adpcm.offsets.currentOffset == 2 && adpcm.offsets.endOffset == 15);
    Check(adpcm.adpcm.coefficients[4] == 1024 && adpcm.adpcm.prevSample[1] == -10 && adpcm.adpcm.predScale == 0x21);
    Check(adpcm.adpcm_loop.prevSample[0] == 20 && adpcm.offsets.loopOffset == 2);
    memory.Write8(source,0x80); Reject([&] { driver.PlayVoice(1,source,32000,2,8,false); });
    // Production memory intercepts route numeric volume/pitch registers without swapping twice.
    memory.SetAudioDriver(&driver);
    constexpr U32 regs = wiiu::AudioDriver::kChannelBase;
    memory.Write16(regs,0x4000); Check(voice.envelope.volume == 0x4000 && memory.Read16(regs) == 0x4000);
    memory.Write32(regs+4,0x18000); Check(voice.ratio == 1.5f && memory.Read32(regs+4) == 0x18000);
    Check(memory.Read32(regs+28) == 1);
    memory.Write32(regs+24,4); Check(!driver.IsPlaying(0));
    Reject([&] { memory.Write32(regs,0x8001); });
    Reject([&] { driver.SetPitch(0,20); });
    memory.LoadImage(source,sine);
    memory.Write32(regs+8,source); memory.Write32(regs+12,static_cast<U32>(sine.size()));
    memory.Write32(regs+16,16000); memory.Write32(regs+20,1); memory.Write32(regs+24,3);
    Check(driver.IsPlaying(0) && voice.offsets.loopingEnabled == 1 && voice.ratio == 0.75f);
    wiiu::hle::HLEServiceManager services;
    Check(services.csndPlaySound(2,source,32000,1,512,false) != 0);
    services.AttachAudioDriver(driver); Check(services.csndInit() == 0 && services.dspInit() == 0);
    Check(services.csndPlaySound(2,source,32000,1,512,false) == 0);
    Check(services.csndIsChannelPlaying(2) && !services.dspIsChannelReady(2));
    Check(services.DSP_PlayBuffer(15,source,32000,1,512,false) == 0 && driver.IsPlaying(15));
    Check(services.DSP_PlayBuffer(16,source,32000,1,512,false) != 0);
    Check(services.dspExit() == 0 && !driver.IsPlaying(15));
    Check(!services.csndIsChannelPlaying(2) && services.dspIsChannelReady(2));
    memory.SetAudioDriver(nullptr); driver.Shutdown();
    Check(!ax_mock::initialized && ax_mock::frame_callback == nullptr && allocations.size() == 1);
    Check(ax_mock::freed == 16);
    ax_mock::acquired = 0; ax_mock::fail_after = 3;
    Check(!driver.Initialize() && !ax_mock::initialized && ax_mock::freed == 19);
    ax_mock::fail_after = -1; ax_mock::fail_registration = true;
    Check(!driver.Initialize() && !ax_mock::initialized); ax_mock::fail_registration = false;
    std::cout << "PCM sine, DMA endian/alignment, AX configuration, ADPCM, lifetime, HLE and MMIO tests passed.\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
