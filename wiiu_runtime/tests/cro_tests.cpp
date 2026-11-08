#include "cro_loader.hpp"
#include "hle_services.hpp"
#include "wiiu_memory.hpp"
#include "pica200.hpp"
#include <vpad/input.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <source_location>
#include <new>

namespace {
using U32 = std::uint32_t;
constexpr U32 base = 0x08100000u, text = 0x400u;
void Check(bool value, std::source_location location = std::source_location::current()) {
    if (!value) throw std::runtime_error("CRO test failed at line " + std::to_string(location.line()));
}
void Put(std::vector<std::uint8_t>& bytes, U32 at, U32 value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = static_cast<std::uint8_t>(value >> (8u * i));
}
void H(std::vector<std::uint8_t>& b, U32 index, U32 value) { Put(b, 0x80u + index * 4u, value); }
void String(std::vector<std::uint8_t>& b, U32 at, const char* value) {
    std::memcpy(b.data() + at, value, std::strlen(value) + 1);
}
std::vector<std::uint8_t> Image(unsigned type = 2, U32 addend = 4,
    const char* name = "test", const char* exported = "function", const char* imported = "external") {
    std::vector<std::uint8_t> b(0x600);
    H(b, 0, 0x304F5243); H(b, 4, 0x600); H(b, 5, 16);
    H(b, 12, text); H(b, 13, 0x40); H(b, 14, 0x580); H(b, 15, 16);
    H(b, 16, 0x280); H(b, 17, 32); String(b, 0x280, name);
    H(b, 18, 0x140); H(b, 19, 4);
    constexpr std::array<U32, 4> offsets{text, 0x500, 0x580, 0};
    for (U32 i = 0; i < 4; ++i) {
        Put(b, 0x140 + i * 12, offsets[i]); Put(b, 0x144 + i * 12, i < 2 ? 0x40u : 16u);
        Put(b, 0x148 + i * 12, i);
    }
    H(b, 20, 0x170); H(b, 21, 1); Put(b, 0x170, 0x300); Put(b, 0x174, 4u << 4);
    H(b, 24, 0x300); H(b, 25, 64); String(b, 0x300, exported);
    H(b, 30, 0x190); H(b, 31, 1);
    H(b, 32, 0x180); H(b, 33, 1); Put(b, 0x180, 0x340); Put(b, 0x184, 0x190);
    H(b, 38, 0x340); H(b, 39, 64); String(b, 0x340, imported);
    b[0x194] = static_cast<std::uint8_t>(type); b[0x195] = 1; Put(b, 0x198, addend);
    Put(b, text, 0xEBFFFFFE); Put(b, text + 4, 0xE12FFF1E);
    Put(b, 0x580, 0x11223344);
    return b;
}
U32 ThumbTarget(wiiu::WiiUMemory& memory, U32 pc) {
    const U32 hi = memory.Read16(pc), lo = memory.Read16(pc + 2), s = (hi >> 10) & 1u;
    U32 bits = (s << 24) | (((~((lo >> 13) ^ s)) & 1u) << 23) |
        (((~((lo >> 11) ^ s)) & 1u) << 22) | ((hi & 0x3FFu) << 12) | ((lo & 0x7FFu) << 1);
    if (s != 0) bits |= 0xFE000000u;
    return ((lo & 0x1000u) != 0 ? pc + 4 : (pc + 4) & ~3u) + bits;
}
struct PolicyProbe {
    bool allow{true}, throws{false};
    unsigned calls{};
    std::size_t segment_count{};
    U32 load_address{};
};
bool ProbePolicy(void* context, const wiiu::CROModule& module) {
    auto& probe = *static_cast<PolicyProbe*>(context);
    ++probe.calls;
    probe.segment_count = module.segments.size();
    probe.load_address = module.load_address;
    if (probe.throws) throw std::runtime_error("policy refused");
    return probe.allow;
}
constexpr const char* not_translated = "CRO module code is not translated";
} // namespace

// Host tests exercise production guest memory and its endian-safe accessors.
void* MEMAllocFromDefaultHeapEx(U32 size, std::int32_t alignment) {
    return ::operator new(size, std::align_val_t{static_cast<std::size_t>(alignment)}, std::nothrow);
}
void MEMFreeToDefaultHeap(void* pointer) { ::operator delete(pointer, std::align_val_t{0x1000}); }
void OSReport(const char*, ...) {}
void wiiu::Pica200::WriteMMIO(U32, U32, std::uint8_t) { throw std::runtime_error("Unexpected MMIO"); }
std::int32_t VPADRead(VPADChan, VPADStatus*, U32, VPADReadError* error) {
    *error = VPAD_READ_NO_SAMPLES; return 0;
}
void VPADGetTPCalibratedPointEx(VPADChan, VPADTouchPadResolution, VPADTouchData* out,
                               const VPADTouchData* input) { *out = *input; }

int main() try {
    wiiu::WiiUMemory memory;
    wiiu::CROLoader loader;
    Check(loader.RegisterSymbol("external", 0x08001234));
    auto bytes = Image(); wiiu::CROModule module;
    Check(loader.LoadModule(memory, bytes.data(), bytes.size(), base, module));
    Check(memory.Read32(base + text) == 0x08001238);
    Check(memory.Read8(base + text) == 0x38 && memory.Read8(base + text + 3) == 0x08);
    Check(memory.Read32(base + 0x580) == 0x11223344 && memory.Read32(base + 0x1000) == 0);
    Check(module.IsExecutable(base + text) && !module.IsExecutable(base + 0x580));
    U32 value{}; Check(loader.FindSymbol("function", value) && value == base + text + 4);
    Check(!memory.ReserveDynamicRange(base, 0x1000));
    Check(loader.UnloadModule(memory, base) && memory.Read32(base + text) == 0);
    Check(!loader.FindSymbol("function", value));
    for (unsigned type : {3u, 42u}) {
        bytes = Image(type, 4);
        Check(loader.LoadModule(memory, bytes.data(), bytes.size(), base, module));
        const U32 relative = 0x08001238u - (base + text);
        Check(memory.Read32(base + text) == (type == 3 ? relative : (relative & 0x7FFFFFFFu) | 0x80000000u));
        Check(loader.UnloadModule(memory, base));
    }
    // Absolute aliases, negative addends, signed ARM/Thumb branches and interworking.
    for (unsigned type : {2u, 38u, 41u, 28u, 29u, 10u}) {
        for (bool backwards : {false, true}) {
            wiiu::CROLoader link;
            const auto target = backwards ? base - 0x100u : base + 0x800u;
            const auto symbol = target | (type == 10 ? 1u : 0u);
            Check(link.RegisterSymbol("external", symbol));
            const U32 addend = type == 10 ? 0xFFFFFFFCu : (type == 28 || type == 29 ? 0xFFFFFFF8u : 0xFFFFFFFCu);
            bytes = Image(type, addend);
            if (type == 10) Put(bytes, text, 0xF800F000);
            Check(link.LoadModule(memory, bytes.data(), bytes.size(), base, module));
            if (type == 10) Check(ThumbTarget(memory, base + text) == target);
            else if (type == 28 || type == 29) {
                U32 displacement = (memory.Read32(base + text) & 0xFFFFFFu) << 2;
                if ((displacement & 0x02000000u) != 0) displacement |= 0xFC000000u;
                Check(base + text + 8 + displacement == target);
            } else Check(memory.Read32(base + text) == symbol - 4);
            Check(link.UnloadModule(memory, base));
        }
    }
    for (bool thumb_source : {false, true}) {
        wiiu::CROLoader link;
        Check(link.RegisterSymbol("external", (base + 0x800u) | (thumb_source ? 0u : 1u)));
        bytes = Image(thumb_source ? 10 : 28, thumb_source ? 0xFFFFFFFCu : 0xFFFFFFF8u);
        if (thumb_source) { Put(bytes, text + 2, 0xE800F000); Put(bytes, 0x190, 2u << 4); }
        Check(link.LoadModule(memory, bytes.data(), bytes.size(), base, module));
        if (thumb_source) Check(ThumbTarget(memory, base + text + 2) == base + 0x800);
        else Check((memory.Read32(base + text) & 0xFE000000u) == 0xFA000000u);
        Check(link.UnloadModule(memory, base));
    }
    // Malformed input must leave guest bytes, exports and reservations untouched.
    for (unsigned fault = 0; fault < 9; ++fault) {
        bytes = Image();
        if (fault == 0) H(bytes, 0, 0);
        if (fault == 1) H(bytes, 31, 0xFFFFFFFF);
        if (fault == 2) Put(bytes, 0x190, 15);
        if (fault == 3) bytes[0x195] = 0;
        if (fault == 4) bytes[0x194] = 39;
        if (fault == 5) String(bytes, 0x340, "missing");
        if (fault == 6) Put(bytes, 0x180, 0x600);
        if (fault == 7) Put(bytes, 0x190, 0x3Eu << 4);
        if (fault == 8) { bytes[0x194] = 28; Put(bytes, text, 0xE1A00000); }
        memory.Write32(base + text, 0xCAFEBABE);
        Check(!loader.LoadModule(memory, bytes.data(), bytes.size(), base, module));
        Check(memory.Read32(base + text) == 0xCAFEBABE && !loader.FindSymbol("function", value));
        Check(memory.ReserveDynamicRange(base, 0x2000));
        Check(memory.ReleaseDynamicRange(base, 0x2000));
    }
    // Internal relocations use the selected segment base, not the image base.
    for (unsigned type : {10u, 28u, 29u}) {
        wiiu::CROLoader far;
        Check(far.RegisterSymbol("external", type == 10 ? 0x0FFF0001u : 0x0FFF0000u));
        bytes = Image(type, type == 10 ? 0xFFFFFFFCu : 0xFFFFFFF8u);
        if (type == 10) Put(bytes, text, 0xF800F000u);
        Check(!far.LoadModule(memory, bytes.data(), bytes.size(), base, module));
    }
    bytes = Image(); H(bytes, 42, 0x1C0); H(bytes, 43, 1);
    Put(bytes, 0x1C0, 8u << 4); bytes[0x1C4] = 2; bytes[0x1C5] = 2; Put(bytes, 0x1C8, 4);
    Check(loader.LoadModule(memory, bytes.data(), bytes.size(), base, module));
    Check(memory.Read32(base + text + 8) == base + 0x584);
    auto consumer = Image(2, 0, "consumer", "consumer_fn", "function");
    Check(loader.LoadModule(memory, consumer.data(), consumer.size(), base + 0x4000, module));
    Check(memory.Read32(base + 0x4400) == base + text + 4);
    Check(!loader.UnloadModule(memory, base));
    Check(loader.UnloadModule(memory, base + 0x4000) && loader.UnloadModule(memory, base));
    // Indexed and anonymous imports select symbols from a named provider module.
    for (bool anonymous : {false, true}) {
        bytes = Image(); H(bytes, 22, 0x178); H(bytes, 23, 1); Put(bytes, 0x178, 4u << 4);
        Check(loader.LoadModule(memory, bytes.data(), bytes.size(), base, module));
        consumer = Image(2, 0, "consumer", "consumer_fn"); H(consumer, 33, 0);
        H(consumer, 28, 0x1A0); H(consumer, 29, 1); String(consumer, 0x340, "test");
        Put(consumer, 0x1A0, 0x340);
        const U32 record = anonymous ? 0x1B8u : 0x1B0u;
        H(consumer, anonymous ? 36 : 34, record); H(consumer, anonymous ? 37 : 35, 1);
        Put(consumer, 0x1A0 + (anonymous ? 12 : 4), record);
        Put(consumer, 0x1A0 + (anonymous ? 16 : 8), 1);
        Put(consumer, record, anonymous ? 4u << 4 : 0); Put(consumer, record + 4, 0x190);
        Check(loader.LoadModule(memory, consumer.data(), consumer.size(), base + 0x4000, module));
        Check(memory.Read32(base + 0x4400) == base + text + 4);
        Check(!loader.UnloadModule(memory, base));
        Check(loader.UnloadModule(memory, base + 0x4000) && loader.UnloadModule(memory, base));
    }
    // Code policy: an opt-in gate after relocation, before the module becomes visible.
    {
        PolicyProbe probe{};
        wiiu::CROLoader gated;
        Check(gated.RegisterSymbol("external", 0x08001234));
        gated.SetCodePolicy(ProbePolicy, &probe);
        bytes = Image();
        Check(gated.LoadModule(memory, bytes.data(), bytes.size(), base, module));
        Check(probe.calls == 1 && probe.segment_count == 4 && probe.load_address == base);
        Check(gated.UnloadModule(memory, base));
        probe.allow = false;
        memory.Write32(base + text, 0xCAFEBABE);
        bytes = Image();
        Check(!gated.LoadModule(memory, bytes.data(), bytes.size(), base, module));
        Check(gated.LastError() == not_translated && probe.calls == 2);
        Check(memory.Read32(base + text) == 0xCAFEBABE && !gated.FindSymbol("function", value));
        Check(memory.ReserveDynamicRange(base, 0x2000));
        Check(memory.ReleaseDynamicRange(base, 0x2000));
        probe.allow = true; probe.throws = true;
        Check(!gated.LoadModule(memory, bytes.data(), bytes.size(), base, module));
        Check(gated.LastError() == not_translated && !gated.FindSymbol("function", value));
        Check(memory.Read32(base + text) == 0xCAFEBABE);
        gated.SetCodePolicy(nullptr, nullptr);
        Check(gated.LoadModule(memory, bytes.data(), bytes.size(), base, module));
        Check(probe.calls == 3 && memory.Read32(base + text) == 0x08001238);
        Check(gated.UnloadModule(memory, base));
    }
    // A rejected second module leaves the first loaded, exported and unloadable.
    {
        PolicyProbe probe{};
        wiiu::CROLoader pair;
        Check(pair.RegisterSymbol("external", 0x08001234));
        pair.SetCodePolicy(ProbePolicy, &probe);
        bytes = Image();
        Check(pair.LoadModule(memory, bytes.data(), bytes.size(), base, module));
        probe.allow = false;
        auto second = Image(2, 0, "second", "second_fn", "external");
        Check(!pair.LoadModule(memory, second.data(), second.size(), base + 0x4000, module));
        Check(pair.LastError() == not_translated && !pair.FindSymbol("second_fn", value));
        Check(pair.FindSymbol("function", value) && value == base + text + 4);
        Check(memory.Read32(base + text) == 0x08001238 && !memory.ReserveDynamicRange(base, 0x1000));
        Check(memory.ReserveDynamicRange(base + 0x4000, 0x1000));
        Check(memory.ReleaseDynamicRange(base + 0x4000, 0x1000));
        Check(pair.UnloadModule(memory, base) && memory.Read32(base + text) == 0);
        Check(!pair.FindSymbol("function", value));
    }
    // Shared reservations prevent module/heap collisions and permit reuse after free.
    Check(memory.ReserveDynamicRange(base, 0x2000)); bytes = Image();
    Check(!loader.LoadModule(memory, bytes.data(), bytes.size(), base, module));
    Check(memory.ReleaseDynamicRange(base, 0x2000));
    wiiu::hle::HLEServiceManager services;
    Check(services.GetCROLoader().RegisterSymbol("external", 0x08001234));
    constexpr U32 source = base + 0x10000;
    memory.LoadImage(source, bytes);
    Check(services.RO_RegisterModule(memory, source, static_cast<U32>(bytes.size())) == 0);
    Check(services.RegisteredROHashes(source).size() == 128);
    Check(services.RO_LoadModule(memory, source, static_cast<U32>(bytes.size()), base, module) == 0);
    Check(memory.Read32(base + text) == 0x08001238);
    Check(services.RO_UnloadModule(memory, base) == 0);
    std::vector<std::uint8_t> crr(0x1000);
    Put(crr, 0, 0x30525243); Put(crr, 0x344, 0x1000); Put(crr, 0x350, 0x360); Put(crr, 0x354, 1);
    crr[0x360] = 0xAB; memory.LoadImage(source, crr);
    Check(services.RO_RegisterModule(memory, source, 0x1000) == 0);
    Check(services.RegisteredROHashes(source).size() == 32 && services.RegisteredROHashes(source)[0] == 0xAB);
    Put(crr, 0x354, 0xFFFFFFFF); memory.LoadImage(source, crr);
    Check(services.RO_RegisterModule(memory, source, 0x1000) != 0);
    std::cout << "CRO mapping, imports, relocations, bounds, reservations and RO HLE tests passed.\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n'; return 1;
}
