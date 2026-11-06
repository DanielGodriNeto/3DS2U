#include "hid_manager.hpp"
#include "hle_services.hpp"
#include "pica200.hpp"
#include "wiiu_memory.hpp"

#include <coreinit/memdefaultheap.h>

#include <malloc.h>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <iterator>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <vector>

namespace {

struct TestFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void Fail(const std::string& detail, std::source_location where) {
    throw TestFailure(detail + " (line " + std::to_string(where.line()) + ")");
}

void Check(bool condition, std::source_location where = std::source_location::current()) {
    if (!condition) Fail("check failed", where);
}

// Exact dynamic type match: std::out_of_range derives from std::logic_error.
template <typename Error, typename Action>
void ExpectThrow(Action&& action, std::string_view needle,
                 std::source_location where = std::source_location::current()) {
    try {
        action();
    } catch (const std::exception& error) {
        if (typeid(error) != typeid(Error)) {
            Fail(std::string("unexpected exception type, what() = \"") + error.what() + "\"", where);
        }
        if (std::string_view{error.what()}.find(needle) == std::string_view::npos) {
            Fail(std::string("message \"") + error.what() + "\" lacks \"" + std::string(needle) + "\"",
                 where);
        }
        return;
    }
    Fail("no exception, expected message containing \"" + std::string(needle) + "\"", where);
}

std::vector<std::uint8_t> Pattern(std::size_t size, std::size_t seed) {
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::uint8_t>((i * 7u + seed) & 0xFFu);
    }
    return bytes;
}

void ExpectBytes(const wiiu::WiiUMemory& memory, std::uint32_t address,
                 std::span<const std::uint8_t> expected,
                 std::source_location where = std::source_location::current()) {
    for (std::size_t i = 0; i < expected.size(); ++i) {
        Check(memory.Read8(address + static_cast<std::uint32_t>(i)) == expected[i], where);
    }
}

void ExpectZero(const wiiu::WiiUMemory& memory, std::uint32_t address, std::size_t size,
                std::source_location where = std::source_location::current()) {
    for (std::size_t i = 0; i < size; ++i) {
        Check(memory.Read8(address + static_cast<std::uint32_t>(i)) == 0, where);
    }
}

std::uint8_t ByteAt(const wiiu::WiiUMemory& memory, std::uint32_t address) {
    return std::to_integer<std::uint8_t>(*static_cast<const std::byte*>(memory.GetPointer(address)));
}

const std::byte* Backing(const wiiu::WiiUMemory& memory, std::uint32_t address) {
    return static_cast<const std::byte*>(memory.GetPointer(address));
}

void ContiguousSectionsKeepBytesAndPackedBacking() {
    wiiu::WiiUMemory memory;
    const auto text = Pattern(0x1800, 3);
    const auto rodata = Pattern(0x0400, 11);
    const auto data = Pattern(0x0800, 19);
    memory.MapImageRange(0x00100000u, 0x0000u, 0x2000u);
    memory.MapImageRange(0x00102000u, 0x2000u, 0x1000u);
    memory.MapImageRange(0x00103000u, 0x3000u, 0x1000u);
    memory.MapImageRange(0x00104000u, 0x4000u, 0x2000u);
    memory.LoadImage(0x00100000u, text);
    memory.LoadImage(0x00102000u, rodata);
    memory.LoadImage(0x00103000u, data);
    memory.ClearRange(0x00104000u, 0x2000u);
    ExpectBytes(memory, 0x00100000u, text);
    ExpectBytes(memory, 0x00102000u, rodata);
    ExpectBytes(memory, 0x00103000u, data);
    ExpectZero(memory, 0x00100000u + static_cast<std::uint32_t>(text.size()), 0x2000u - text.size());
    ExpectZero(memory, 0x00102000u + static_cast<std::uint32_t>(rodata.size()), 0x1000u - rodata.size());
    ExpectZero(memory, 0x00103000u + static_cast<std::uint32_t>(data.size()), 0x1000u - data.size());
    ExpectZero(memory, 0x00104000u, 0x2000u);
    Check(Backing(memory, 0x00102000u) == Backing(memory, 0x00100000u) + 0x2000);
    Check(Backing(memory, 0x00103000u) == Backing(memory, 0x00100000u) + 0x3000);
    Check(Backing(memory, 0x00104000u) == Backing(memory, 0x00100000u) + 0x4000);
}

void GappedMappingsReadBothSidesAndRejectHole() {
    wiiu::WiiUMemory memory;
    const auto low = Pattern(0x1000, 5);
    const auto high = Pattern(0x1000, 13);
    memory.MapImageRange(0x00100000u, 0x0000u, 0x1000u);
    memory.MapImageRange(0x00200000u, 0x1000u, 0x1000u);
    memory.LoadImage(0x00100000u, low);
    memory.LoadImage(0x00200000u, high);
    ExpectBytes(memory, 0x00100000u, low);
    ExpectBytes(memory, 0x00200000u, high);
    ExpectThrow<std::out_of_range>([&] { (void)memory.Read8(0x00180000u); }, "unmapped");
}

void MappingEdgesReadLastByteAndRejectPastEnd() {
    wiiu::WiiUMemory memory;
    const auto bytes = Pattern(0x1000, 7);
    memory.MapImageRange(0x00100000u, 0x0000u, 0x1000u);
    memory.LoadImage(0x00100000u, bytes);
    Check(memory.Read8(0x00100000u) == bytes.front());
    Check(memory.Read8(0x00100FFFu) == bytes.back());
    ExpectThrow<std::out_of_range>([&] { (void)memory.Read8(0x00101000u); }, "FCRAM virtual window");
    Check(static_cast<const std::byte*>(memory.GetPointer(0x00101000u, 0)) ==
          Backing(memory, 0x00100FFFu) + 1);
}

void StraddlingAccessesThrowWithReason() {
    wiiu::WiiUMemory memory;
    memory.MapImageRange(0x00100000u, 0x0000u, 0x1000u);
    memory.MapImageRange(0x00101000u, 0x1000u, 0x1000u);
    ExpectThrow<std::out_of_range>([&] { (void)memory.Read32(0x00100FFEu); }, "straddles");
    memory.MapImageRange(0x00300000u, 0x4000u, 0x1000u);
    ExpectThrow<std::out_of_range>([&] { (void)memory.Read32(0x00300FFEu); }, "straddles");
    memory.MapImageRange(0xFFFFF000u, 0x8000u, 0x1000u);
    ExpectThrow<std::out_of_range>([&] { (void)memory.Read32(0xFFFFFFFEu); }, "straddles");
}

void OverlappingVirtualRangesAreRejected() {
    wiiu::WiiUMemory memory;
    memory.MapImageRange(0x00100000u, 0x0000u, 0x2000u);
    ExpectThrow<std::logic_error>([&] { memory.MapImageRange(0x00101000u, 0x4000u, 0x1000u); },
                                  "Overlapping image mappings");
    ExpectThrow<std::logic_error>([&] { memory.MapImageRange(0x00100000u, 0x0000u, 0x2000u); },
                                  "Overlapping image mappings");
}

void SameBackingWithDifferentVirtualRangeIsRejected() {
    wiiu::WiiUMemory memory;
    memory.MapImageRange(0x00100000u, 0x0000u, 0x1000u);
    ExpectThrow<std::logic_error>([&] { memory.MapImageRange(0x00200000u, 0x0000u, 0x1000u); },
                                  "share FCRAM backing");
}

void AdjacentVirtualAndBackingRangesAreAccepted() {
    wiiu::WiiUMemory memory;
    const auto first = Pattern(0x1000, 2);
    const auto second = Pattern(0x1000, 9);
    memory.MapImageRange(0x00100000u, 0x0000u, 0x1000u);
    memory.MapImageRange(0x00101000u, 0x1000u, 0x1000u);
    memory.LoadImage(0x00100000u, first);
    memory.LoadImage(0x00101000u, second);
    ExpectBytes(memory, 0x00100000u, first);
    ExpectBytes(memory, 0x00101000u, second);
}

void UnalignedImageMappingsAreRejected() {
    wiiu::WiiUMemory memory;
    ExpectThrow<std::out_of_range>([&] { memory.MapImageRange(0x00100800u, 0x0000u, 0x1000u); },
                                   "not page aligned");
    ExpectThrow<std::out_of_range>([&] { memory.MapImageRange(0x00100000u, 0x0800u, 0x1000u); },
                                   "not page aligned");
    ExpectThrow<std::out_of_range>([&] { memory.MapImageRange(0x00100000u, 0x0000u, 0x0800u); },
                                   "not page aligned");
}

void ZeroSizeImageMappingIsRejected() {
    wiiu::WiiUMemory memory;
    ExpectThrow<std::out_of_range>([&] { memory.MapImageRange(0x00100000u, 0x0000u, 0x0000u); },
                                   "size is zero");
}

void VirtualRangeWrappingPastFourGiBIsRejected() {
    wiiu::WiiUMemory memory;
    ExpectThrow<std::out_of_range>([&] { memory.MapImageRange(0xFFFFF000u, 0x0000u, 0x2000u); },
                                   "32-bit guest address space");
}

void MappingEndingAtFourGiBIsAccepted() {
    wiiu::WiiUMemory memory;
    const auto bytes = Pattern(0x1000, 17);
    memory.MapImageRange(0xFFFFF000u, 0x0000u, 0x1000u);
    memory.LoadImage(0xFFFFF000u, bytes);
    Check(memory.Read8(0xFFFFFFFFu) == bytes.back());
}

void BackingOutsideFcramIsRejected() {
    wiiu::WiiUMemory memory;
    ExpectThrow<std::out_of_range>([&] { memory.MapImageRange(0x00100000u, 0x08000000u, 0x1000u); },
                                   "exceeds FCRAM backing");
    ExpectThrow<std::out_of_range>([&] { memory.MapImageRange(0x00100000u, 0xFFFFF000u, 0x2000u); },
                                   "exceeds FCRAM backing");
    ExpectThrow<std::out_of_range>([&] { memory.MapImageRange(0x00100000u, 0x07FFF000u, 0x2000u); },
                                   "exceeds FCRAM backing");
}

void CanonicalWindowRuleAllowsOnlyIdentityMappings() {
    wiiu::WiiUMemory memory;
    ExpectThrow<std::logic_error>([&] { memory.MapImageRange(0x08100000u, 0x0000u, 0x1000u); },
                                  "canonical FCRAM window");
    const auto bytes = Pattern(0x1000, 23);
    memory.MapImageRange(0x08002000u, 0x2000u, 0x1000u);
    memory.LoadImage(0x08002000u, bytes);
    ExpectBytes(memory, 0x08002000u, bytes);
}

void BackingOverlappingDynamicRangeIsRejected() {
    wiiu::WiiUMemory memory;
    Check(memory.ReserveDynamicRange(0x08010000u, 0x2000u));
    ExpectThrow<std::logic_error>([&] { memory.MapImageRange(0x00100000u, 0x10000u, 0x1000u); },
                                  "overlaps a dynamic range");
}

void MappedBackingIsNeverHandedOutAsHeap() {
    wiiu::WiiUMemory memory;
    memory.MapImageRange(0x00100000u, 0x2000u, 0x1000u);
    Check(memory.GetImageEnd() == 0x08003000u);
    Check(!memory.ReserveDynamicRange(0x08002000u, 0x1000u));
    Check(memory.FindFreeRange(0x1000u) >= 0x08003000u);
    Check(memory.ReserveDynamicRange(0x08003000u, 0x1000u));
}

void MappingAfterHeapMustLieBelowImageEnd() {
    wiiu::WiiUMemory memory;
    Check(memory.ReserveDynamicRange(0x08010000u, 0x1000u));
    ExpectThrow<std::logic_error>([&] { memory.MapImageRange(0x00100000u, 0x20000u, 0x1000u); },
                                  "Reserve images before dynamic");
    memory.MapImageRange(0x00100000u, 0x0000u, 0x1000u);
    ExpectBytes(memory, 0x00100000u, std::vector<std::uint8_t>(0x1000, 0));
}

void RejectedMappingsLeaveStateUnchanged() {
    wiiu::WiiUMemory memory;
    const auto first = Pattern(0x1000, 31);
    const auto second = Pattern(0x1000, 37);
    memory.MapImageRange(0x00100000u, 0x0000u, 0x1000u);
    memory.LoadImage(0x00100000u, first);

    ExpectThrow<std::logic_error>([&] { memory.MapImageRange(0x00200000u, 0x0000u, 0x1000u); },
                                  "share FCRAM backing");
    ExpectThrow<std::out_of_range>([&] { (void)memory.Read8(0x00200000u); }, "unmapped");
    memory.MapImageRange(0x00200000u, 0x1000u, 0x1000u);
    memory.LoadImage(0x00200000u, second);
    ExpectBytes(memory, 0x00200000u, second);
    ExpectBytes(memory, 0x00100000u, first);

    ExpectThrow<std::logic_error>([&] { memory.MapImageRange(0x00100000u, 0x2000u, 0x1000u); },
                                  "Overlapping image mappings");
    ExpectBytes(memory, 0x00100000u, first);
    memory.MapImageRange(0x00300000u, 0x2000u, 0x1000u);
}

void LittleEndianGuestStoresMatchLayout() {
    wiiu::WiiUMemory memory;
    memory.Write32(0x08000100u, 0x11223344u);
    Check(ByteAt(memory, 0x08000100u) == 0x44);
    Check(ByteAt(memory, 0x08000101u) == 0x33);
    Check(ByteAt(memory, 0x08000102u) == 0x22);
    Check(ByteAt(memory, 0x08000103u) == 0x11);
    Check(memory.Read32(0x08000100u) == 0x11223344u);
    memory.Write16(0x08000200u, std::uint16_t{0x5566});
    Check(ByteAt(memory, 0x08000200u) == 0x66);
    Check(ByteAt(memory, 0x08000201u) == 0x55);
    Check(memory.Read16(0x08000200u) == std::uint16_t{0x5566});
}

void ClearRangeThroughImageMappingRoundTrips() {
    wiiu::WiiUMemory memory;
    const auto bytes = Pattern(0x1800, 29);
    const std::span<const std::uint8_t> view{bytes};
    memory.MapImageRange(0x00100000u, 0x1000u, 0x2000u);
    memory.LoadImage(0x00100000u, bytes);
    ExpectBytes(memory, 0x00100000u, bytes);
    memory.ClearRange(0x00101000u, 0x1000u);
    ExpectBytes(memory, 0x00100000u, view.first(0x1000));
    ExpectZero(memory, 0x00101000u, 0x1000u);
}

void ReserveImageRangeUsesMappedBacking() {
    wiiu::WiiUMemory memory;
    memory.MapImageRange(0x00100000u, 0x2000u, 0x3000u);
    memory.ReserveImageRange(0x00100000u, 0x3000u);
    Check(memory.GetImageEnd() == 0x08000000u + 0x2000u + 0x3000u);
}

void IdentityStraddleFallsThroughToCanonical() {
    wiiu::WiiUMemory memory;
    const auto bytes = Pattern(0x2000, 41);
    memory.MapImageRange(0x08000000u, 0x0000u, 0x1000u);
    memory.LoadImage(0x08000000u, bytes);
    const std::uint32_t expected =
        static_cast<std::uint32_t>(bytes[0xFFE]) |
        (static_cast<std::uint32_t>(bytes[0xFFF]) << 8) |
        (static_cast<std::uint32_t>(bytes[0x1000]) << 16) |
        (static_cast<std::uint32_t>(bytes[0x1001]) << 24);
    Check(memory.Read32(0x08000FFEu) == expected);
}

void NonIdentityMappingAliasesCanonicalWindow() {
    wiiu::WiiUMemory memory;
    const auto bytes = Pattern(0x1000, 31);
    memory.MapImageRange(0x00100000u, 0x1000u, 0x1000u);
    memory.LoadImage(0x00100000u, bytes);
    ExpectBytes(memory, 0x08001000u, bytes);
}

struct TestCase {
    std::string_view name;
    void (*body)();
};

const TestCase kTests[] = {
    {"01 contiguous sections keep bytes and packed backing", ContiguousSectionsKeepBytesAndPackedBacking},
    {"02 gapped mappings read both sides and reject hole", GappedMappingsReadBothSidesAndRejectHole},
    {"03 mapping edges read last byte and reject past end", MappingEdgesReadLastByteAndRejectPastEnd},
    {"04 straddling accesses throw with reason", StraddlingAccessesThrowWithReason},
    {"05 overlapping virtual ranges are rejected", OverlappingVirtualRangesAreRejected},
    {"06 same backing with different virtual range is rejected", SameBackingWithDifferentVirtualRangeIsRejected},
    {"07 adjacent virtual and backing ranges are accepted", AdjacentVirtualAndBackingRangesAreAccepted},
    {"08 unaligned image mappings are rejected", UnalignedImageMappingsAreRejected},
    {"09 zero-size image mapping is rejected", ZeroSizeImageMappingIsRejected},
    {"10 virtual range wrapping past 4 GiB is rejected", VirtualRangeWrappingPastFourGiBIsRejected},
    {"11 mapping ending at 4 GiB is accepted", MappingEndingAtFourGiBIsAccepted},
    {"12 backing outside FCRAM is rejected", BackingOutsideFcramIsRejected},
    {"13 canonical window rule allows only identity mappings", CanonicalWindowRuleAllowsOnlyIdentityMappings},
    {"14 backing overlapping dynamic range is rejected", BackingOverlappingDynamicRangeIsRejected},
    {"15 rejected mappings leave state unchanged", RejectedMappingsLeaveStateUnchanged},
    {"16 little-endian guest stores match layout", LittleEndianGuestStoresMatchLayout},
    {"17 ClearRange through image mapping round-trips", ClearRangeThroughImageMappingRoundTrips},
    {"18 ReserveImageRange uses mapped backing", ReserveImageRangeUsesMappedBacking},
    {"19 identity straddle falls through to canonical", IdentityStraddleFallsThroughToCanonical},
    {"20 non-identity mapping aliases canonical window", NonIdentityMappingAliasesCanonicalWindow},
    {"21 mapped backing is never handed out as heap", MappedBackingIsNeverHandedOutAsHeap},
    {"22 mapping after heap must lie below image end", MappingAfterHeapMustLieBelowImageEnd},
};

} // namespace

// Host doubles for platform services linked through WiiUMemory, Pica200, HIDManager and HLE audio.
void* MEMAllocFromDefaultHeapEx(std::uint32_t size, std::int32_t alignment) {
    return _aligned_malloc(static_cast<std::size_t>(size), static_cast<std::size_t>(alignment));
}

void MEMFreeToDefaultHeap(void* pointer) {
    _aligned_free(pointer);
}

void wiiu::Pica200::WriteMMIO(std::uint32_t, std::uint32_t, std::uint8_t) {
    throw std::logic_error("guest memory test reached graphics bridge");
}

std::uint32_t wiiu::HIDManager::ReadRegister(std::uint32_t, unsigned) const {
    throw std::logic_error("guest memory test reached HID bridge");
}

std::uint32_t wiiu::hle::HLEServiceManager::ReadAudioMMIO(std::uint32_t, unsigned, wiiu::AudioDriver*) {
    throw std::logic_error("guest memory test reached audio MMIO read");
}

void wiiu::hle::HLEServiceManager::WriteAudioMMIO(std::uint32_t, std::uint32_t, unsigned,
                                                  wiiu::AudioDriver*) {
    throw std::logic_error("guest memory test reached audio MMIO write");
}

int main() {
    std::vector<std::string_view> failed;
    for (const auto& test : kTests) {
        try {
            test.body();
        } catch (const std::exception& error) {
            std::cerr << "FAILED " << test.name << ": " << error.what() << '\n';
            failed.push_back(test.name);
        }
    }
    if (!failed.empty()) {
        std::cerr << failed.size() << " of " << std::size(kTests) << " guest memory tests failed:\n";
        for (const auto name : failed) std::cerr << "  " << name << '\n';
        return 1;
    }
    std::cout << "Guest memory mapping tests passed.\n";
    return 0;
}
