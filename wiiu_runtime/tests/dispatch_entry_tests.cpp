#include "pica200.hpp"
#include "dispatch_diagnostics.hpp"
#include "wiiu_engine.hpp"

#include <vpad/input.h>
#include <malloc.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
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

void HasBlockInRangeMatchesGuestAddresses() {
    wiiu::WiiUEngine empty;
    Check(!empty.HasBlockInRange(0x00100000u, 0x10u));
    wiiu::WiiUEngine engine;
    engine.RegisterBlock(0x00100000u, ArmBlock);
    engine.RegisterBlock(0x00200001u, ThumbBlock);
    engine.RegisterBlock(0xFFFFFFFCu, ArmBlock);
    Check(engine.HasBlockInRange(0x000FFFF0u, 0x20u));
    Check(engine.HasBlockInRange(0x00200000u, 2u));
    Check(!engine.HasBlockInRange(0x00200001u, 1u));
    Check(!engine.HasBlockInRange(0x000FFFF0u, 0x10u));
    Check(!engine.HasBlockInRange(0x00100000u, 0u));
    Check(engine.HasBlockInRange(0xFFFFFFF0u, 0x20u));
    Check(engine.HasBlockInRange(0x00100004u, 0xFFFFFFFCu));
}

// Guest code page for the diagnostic checks. The real memory backs only the FCRAM window
// and explicitly mapped image ranges, so these addresses need a mapped page.
constexpr std::uint32_t kCodeBase = 0x00100000u;
constexpr std::uint32_t kCodePageSize = 0x1000u;

struct ModuleFixture {
    std::uint32_t begin{};
    std::uint32_t end{};
    const char* name{};
};

const char* LookupModule(void* context, std::uint32_t address) {
    const auto& fixture = *static_cast<const ModuleFixture*>(context);
    if (address < fixture.begin || address >= fixture.end) return nullptr;
    return fixture.name;
}

// Falls through to a PC with no block and does not yield, so the next dispatch misses.
void JumpToMissingBlock(arm11::CPUContext& cpu, wiiu::WiiUMemory&) {
    ++arm_calls;
    cpu.r[15] = kCodeBase + 8u;
}

void MapCodePage(wiiu::WiiUEngine& engine) {
    engine.GetMemory().MapImageRange(kCodeBase, 0u, kCodePageSize);
}

// A miss halts the main CPU and calls no block beyond those that ran before it.
void CheckMissHalted(const wiiu::WiiUEngine& engine, unsigned expected_block_calls) {
    Check(engine.IsHalted());
    Check(arm_calls + thumb_calls == expected_block_calls);
}

std::string FormatDetail(wiiu::MissingBlockKind kind, std::uint32_t pc, bool thumb,
                         std::uint32_t caller_key, bool has_caller, const char* module) {
    char buffer[256]{};
    const auto written = wiiu::FormatMissingBlockDetail(buffer, sizeof buffer, kind, pc, thumb,
                                                        caller_key, has_caller, module);
    Check(written == std::strlen(buffer));
    return buffer;
}

void DetailIsOffByDefault() {
    wiiu::WiiUEngine engine;
    engine.RegisterBlock(kCodeBase, ArmBlock);
    engine.Run(0x08000000u);
    CheckMissHalted(engine, 0);
    Check(Logged("no translated block at PC 0x08000000"));
    Check(!Logged("Missing block detail"));
}

void MissingArmBlockDetailNamesCaller() {
    wiiu::WiiUEngine engine;
    MapCodePage(engine);
    engine.EnableMissingBlockDiagnostics(true);
    engine.RegisterBlock(kCodeBase, JumpToMissingBlock);
    engine.Run(kCodeBase);
    CheckMissHalted(engine, 1);
    Check(Logged("no translated block at PC 0x00100008"));
    Check(Logged("Missing block detail: kind=NoTranslatedBlock pc=0x00100008 mode=ARM caller=0x00100000 module=-"));
}

void MissingThumbBlockDetailReportsMode() {
    wiiu::WiiUEngine engine;
    MapCodePage(engine);
    engine.EnableMissingBlockDiagnostics(true);
    engine.RegisterBlock(kCodeBase, ArmBlock);
    engine.Run(kCodeBase + 1u);
    CheckMissHalted(engine, 0);
    Check(Logged("Missing block detail: kind=NoTranslatedBlock pc=0x00100000 mode=Thumb caller=- module=-"));
}

void MisalignedArmTargetIsInvalidControlFlow() {
    wiiu::WiiUEngine engine;
    MapCodePage(engine);
    engine.EnableMissingBlockDiagnostics(true);
    engine.RegisterBlock(kCodeBase, ArmBlock);
    engine.Run(kCodeBase + 2u);
    CheckMissHalted(engine, 0);
    Check(Logged("Missing block detail: kind=MisalignedTarget pc=0x00100002 mode=ARM caller=- module=-"));
}

void UnmappedTargetIsReported() {
    wiiu::WiiUEngine engine;
    engine.EnableMissingBlockDiagnostics(true);
    engine.RegisterBlock(kCodeBase, ArmBlock);
    Check(!engine.GetMemory().IsMapped(0xF0000000u));
    engine.Run(0xF0000000u);
    CheckMissHalted(engine, 0);
    Check(Logged("Missing block detail: kind=UnmappedTarget pc=0xF0000000 mode=ARM caller=- module=-"));
}

void CroModuleMissNamesModule() {
    ModuleFixture module{kCodeBase, kCodeBase + kCodePageSize, "libfoo.cro"};
    wiiu::WiiUEngine engine;
    MapCodePage(engine);
    engine.SetModuleLookup(LookupModule, &module);
    engine.EnableMissingBlockDiagnostics(true);
    engine.RegisterBlock(kCodeBase, JumpToMissingBlock);
    engine.Run(kCodeBase);
    CheckMissHalted(engine, 1);
    Check(Logged("Missing block detail: kind=CroModuleMiss pc=0x00100008 mode=ARM caller=0x00100000 module=libfoo.cro"));
}

void LongModuleNameIsTruncated() {
    const std::string long_name(40, 'm');
    ModuleFixture module{kCodeBase, kCodeBase + kCodePageSize, long_name.c_str()};
    wiiu::WiiUEngine engine;
    MapCodePage(engine);
    engine.SetModuleLookup(LookupModule, &module);
    engine.EnableMissingBlockDiagnostics(true);
    engine.RegisterBlock(kCodeBase, JumpToMissingBlock);
    engine.Run(kCodeBase);
    CheckMissHalted(engine, 1);
    Check(Logged("kind=CroModuleMiss pc=0x00100008 mode=ARM caller=0x00100000 module=" + std::string(31, 'm')));
    Check(!Logged("module=" + std::string(32, 'm')));
    for (const auto& line : logs) Check(line.size() < 512);
}

void IsMappedTracksBackedGuestRanges() {
    wiiu::WiiUEngine engine;
    const auto& memory = engine.GetMemory();
    Check(!memory.IsMapped(kCodeBase));
    MapCodePage(engine);
    Check(memory.IsMapped(kCodeBase));
    Check(memory.IsMapped(kCodeBase + kCodePageSize - 4u, 4u));
    Check(!memory.IsMapped(kCodeBase + kCodePageSize - 2u, 4u));
    Check(!memory.IsMapped(0xF0000000u));
}

void ClassifiesMissingBlocksByRule() {
    using wiiu::MissingBlockKind;
    Check(wiiu::ClassifyMissingBlock(0x00100002u, false, true, false) == MissingBlockKind::MisalignedTarget);
    Check(wiiu::ClassifyMissingBlock(0x00100001u, true, true, false) == MissingBlockKind::MisalignedTarget);
    Check(wiiu::ClassifyMissingBlock(0x00100002u, false, false, false) == MissingBlockKind::MisalignedTarget);
    Check(wiiu::ClassifyMissingBlock(0x00100000u, false, false, true) == MissingBlockKind::UnmappedTarget);
    Check(wiiu::ClassifyMissingBlock(0x00100000u, true, false, false) == MissingBlockKind::UnmappedTarget);
    Check(wiiu::ClassifyMissingBlock(0x00100000u, false, true, true) == MissingBlockKind::CroModuleMiss);
    Check(wiiu::ClassifyMissingBlock(0x00100000u, false, true, false) == MissingBlockKind::NoTranslatedBlock);
    Check(wiiu::ClassifyMissingBlock(0x00100002u, true, true, false) == MissingBlockKind::NoTranslatedBlock);
}

void KindNamesAreStable() {
    using wiiu::MissingBlockKind;
    Check(std::strcmp(wiiu::MissingBlockKindName(MissingBlockKind::MisalignedTarget), "MisalignedTarget") == 0);
    Check(std::strcmp(wiiu::MissingBlockKindName(MissingBlockKind::UnmappedTarget), "UnmappedTarget") == 0);
    Check(std::strcmp(wiiu::MissingBlockKindName(MissingBlockKind::NoTranslatedBlock), "NoTranslatedBlock") == 0);
    Check(std::strcmp(wiiu::MissingBlockKindName(MissingBlockKind::CroModuleMiss), "CroModuleMiss") == 0);
}

void FormatsExactLines() {
    using wiiu::MissingBlockKind;
    Check(FormatDetail(MissingBlockKind::NoTranslatedBlock, 0x00100000u, true, 0x00100001u, true, nullptr) ==
          "[3DS2U] Missing block detail: kind=NoTranslatedBlock pc=0x00100000 mode=Thumb caller=0x00100001 module=-");
    Check(FormatDetail(MissingBlockKind::UnmappedTarget, 0xF0000000u, false, 0u, false, "") ==
          "[3DS2U] Missing block detail: kind=UnmappedTarget pc=0xF0000000 mode=ARM caller=- module=-");
    Check(FormatDetail(MissingBlockKind::CroModuleMiss, 0x00100000u, false, 0u, false, "libfoo.cro") ==
          "[3DS2U] Missing block detail: kind=CroModuleMiss pc=0x00100000 mode=ARM caller=- module=libfoo.cro");
}

void FormatSanitizesAndTruncatesModuleName() {
    using wiiu::MissingBlockKind;
    const std::string prefix =
        "[3DS2U] Missing block detail: kind=CroModuleMiss pc=0x00100000 mode=ARM caller=- module=";
    Check(FormatDetail(MissingBlockKind::CroModuleMiss, 0x00100000u, false, 0u, false, "a\nb\x01") ==
          prefix + "a?b?");
    Check(FormatDetail(MissingBlockKind::CroModuleMiss, 0x00100000u, false, 0u, false,
                       std::string(40, 'm').c_str()) == prefix + std::string(31, 'm'));
}

void FormatHonoursCapacityWithoutOverflow() {
    using wiiu::MissingBlockKind;
    const std::string full =
        "[3DS2U] Missing block detail: kind=NoTranslatedBlock pc=0x00100000 mode=ARM caller=- module=-";
    char untouched = 'x';
    Check(wiiu::FormatMissingBlockDetail(&untouched, 0, MissingBlockKind::NoTranslatedBlock,
                                         0x00100000u, false, 0u, false, nullptr) == 0);
    Check(untouched == 'x');
    Check(wiiu::FormatMissingBlockDetail(&untouched, 1, MissingBlockKind::NoTranslatedBlock,
                                         0x00100000u, false, 0u, false, nullptr) == 0);
    Check(untouched == '\0');
    for (std::size_t capacity = 2; capacity <= full.size() + 1; ++capacity) {
        std::vector<char> buffer(capacity, 'x');
        const auto written = wiiu::FormatMissingBlockDetail(buffer.data(), capacity,
                                                            MissingBlockKind::NoTranslatedBlock,
                                                            0x00100000u, false, 0u, false, nullptr);
        const std::size_t expected = std::min(capacity - 1, full.size());
        Check(written == expected);
        Check(buffer[written] == '\0');
        Check(std::string(buffer.data()) == full.substr(0, expected));
    }
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
    ResetCounters();
    HasBlockInRangeMatchesGuestAddresses();
    ResetCounters();
    DetailIsOffByDefault();
    ResetCounters();
    MissingArmBlockDetailNamesCaller();
    ResetCounters();
    MissingThumbBlockDetailReportsMode();
    ResetCounters();
    MisalignedArmTargetIsInvalidControlFlow();
    ResetCounters();
    UnmappedTargetIsReported();
    ResetCounters();
    CroModuleMissNamesModule();
    ResetCounters();
    LongModuleNameIsTruncated();
    ResetCounters();
    IsMappedTracksBackedGuestRanges();
    ClassifiesMissingBlocksByRule();
    KindNamesAreStable();
    FormatsExactLines();
    FormatSanitizesAndTruncatesModuleName();
    FormatHonoursCapacityWithoutOverflow();
    std::cout << "Runtime entry dispatch and missing-block checks passed.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Dispatch entry test failed: " << error.what() << '\n';
    for (const auto& line : logs) std::cerr << "captured: " << line;
    return 1;
}
