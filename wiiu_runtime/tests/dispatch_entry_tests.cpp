#include "pica200.hpp"
#include "wiiu_engine.hpp"

#include <vpad/input.h>
#include <malloc.h>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Synthetic dispatch checks: blocks never touch guest memory, so the stub
// memory's image-range restrictions do not apply.
namespace {

std::vector<std::string> logs;
unsigned arm_calls = 0;
unsigned thumb_calls = 0;
bool thumb_mode_seen = false;

void Check(bool condition) {
    if (!condition) throw std::runtime_error("Dispatch entry test failed");
}

bool Logged(const std::string& fragment) {
    for (const auto& line : logs) {
        if (line.find(fragment) != std::string::npos) return true;
    }
    return false;
}

// Each block ends its bounded slice so Run() returns after one dispatch.
void ArmBlock(arm11::CPUContext& cpu, wiiu::WiiUMemory&) {
    ++arm_calls;
    cpu.r[15] = 0x00100004u;
    cpu.svc_yield = true;
}

void ThumbBlock(arm11::CPUContext& cpu, wiiu::WiiUMemory&) {
    ++thumb_calls;
    thumb_mode_seen = cpu.thumb;
    cpu.r[15] = 0x00100002u;
    cpu.svc_yield = true;
}

void ResetCounters() {
    logs.clear();
    arm_calls = 0;
    thumb_calls = 0;
    thumb_mode_seen = false;
}

void ArmEntryRunsRegisteredBlock() {
    wiiu::WiiUEngine engine;
    engine.RegisterBlock(0x00100000u, ArmBlock);
    engine.Run(0x00100000u);
    Check(arm_calls == 1);
    Check(!engine.IsHalted());
    Check(engine.GetCPU().r[15] == 0x00100004u);
    Check(!engine.GetCPU().thumb);
}

void MissingArmEntryHaltsWithoutCall() {
    wiiu::WiiUEngine engine;
    engine.RegisterBlock(0x00100000u, ArmBlock);
    engine.Run(0x08000000u);
    Check(engine.IsHalted());
    Check(arm_calls == 0);
    Check(engine.GetCPU().r[15] == 0x08000000u);
    Check(Logged("no translated block at PC 0x08000000"));
    Check(Logged("mode ARM"));
    Check(Logged("lookup key 0x08000000"));
    Check(Logged("1 blocks registered"));
}

void ThumbEntryRunsTaggedBlockInThumbMode() {
    wiiu::WiiUEngine engine;
    engine.RegisterBlock(0x00100001u, ThumbBlock);
    engine.Run(0x00100001u);
    Check(thumb_calls == 1 && thumb_mode_seen);
    Check(!engine.IsHalted());
    Check(engine.GetCPU().r[15] == 0x00100002u);
    Check(engine.GetCPU().thumb);
}

void MissingThumbEntryReportsMode() {
    wiiu::WiiUEngine engine;
    engine.RegisterBlock(0x00100000u, ArmBlock);
    engine.Run(0x00100001u);
    Check(engine.IsHalted());
    Check(arm_calls == 0 && thumb_calls == 0);
    Check(Logged("no translated block at PC 0x00100000"));
    Check(Logged("mode Thumb"));
    Check(Logged("lookup key 0x00100001"));
}

} // namespace

// Host doubles for platform services linked through SvcHandler, WiiUMemory and HIDManager.
void* MEMAllocFromDefaultHeapEx(std::uint32_t size, std::int32_t alignment) {
    return _aligned_malloc(size, static_cast<std::size_t>(alignment));
}
void MEMFreeToDefaultHeap(void* pointer) { _aligned_free(pointer); }
void wiiu::Pica200::WriteMMIO(std::uint32_t, std::uint32_t, std::uint8_t) {
    throw std::logic_error("Dispatch entry test reached graphics bridge");
}
std::int32_t VPADRead(VPADChan, VPADStatus*, std::uint32_t, VPADReadError* error) {
    *error = VPAD_READ_NO_SAMPLES;
    return 0;
}
void VPADGetTPCalibratedPointEx(VPADChan, VPADTouchPadResolution,
                               VPADTouchData*, const VPADTouchData*) {}

// Host double for the platform logger; captures the exact runtime diagnostic text.
void OSReport(const char* format, ...) {
    char message[512]{};
    va_list arguments;
    va_start(arguments, format);
    const auto length = std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(message)) {
        throw std::runtime_error("Dispatch entry test log overflow");
    }
    logs.emplace_back(message);
}

int main() try {
    ResetCounters();
    ArmEntryRunsRegisteredBlock();
    ResetCounters();
    MissingArmEntryHaltsWithoutCall();
    ResetCounters();
    ThumbEntryRunsTaggedBlockInThumbMode();
    ResetCounters();
    MissingThumbEntryReportsMode();
    std::cout << "Runtime entry dispatch and missing-block checks passed.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Dispatch entry test failed: " << error.what() << '\n';
    for (const auto& line : logs) std::cerr << "captured: " << line;
    return 1;
}
