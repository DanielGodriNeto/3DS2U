#include "pica200.hpp"
#include "gx2_manager.hpp"
#include "wiiu_memory.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using wiiu::Pica200;
using wiiu::WiiUMemory;

void Check(bool value) {
    if (!value) throw std::runtime_error("PICA200 clear-color regression");
}

// Exact message check: the address must appear in hex, and nothing else may differ.
template <typename Write>
bool ThrowsUnsupportedAt(Write write, const std::string& hex) {
    try {
        write();
    } catch (const std::invalid_argument& error) {
        return std::string(error.what()) == "Unsupported PICA200 register write at " + hex;
    }
    return false;
}

// Valid three-vertex MVP triangle, configured only through supported registers.
void ConfigureTriangle(Pica200& pica) {
    pica.WriteMMIO(0x1EF00200u, 0x08000000u);
    pica.WriteMMIO(0x1EF00204u, 28u);
    pica.WriteMMIO(0x1EF00208u, 3u);
}

void TestDefaultModeCountsWithoutChangingBehaviour() {
    wiiu::gx2::GX2Manager graphics;
    Pica200 pica(graphics);
    WiiUMemory memory(pica);
    ConfigureTriangle(pica);
    Check(pica.UnsupportedWriteCount() == 0 && !pica.HasUnsupportedWrite());

    pica.WriteMMIO(0x1EF00006u, 0xFFFFFFFFu); // Misaligned in-window: ignored.
    Check(pica.UnsupportedWriteCount() == 1 && pica.HasUnsupportedWrite());
    Check(pica.FirstUnsupportedAddress() == 0x1EF00006u);
    Check(pica.GetRegisterValue(0x1EF00006u) == 0);

    pica.WriteMMIO(0x1EF00010u, 0x1234u); // Unknown register: still shadowed.
    Check(pica.UnsupportedWriteCount() == 2);
    Check(pica.FirstUnsupportedAddress() == 0x1EF00006u);
    Check(pica.GetRegisterValue(0x1EF00010u) == 0x1234u);

    pica.WriteMMIO(0x1EF0020Cu, 2u); // Draw value other than 1: ignored, value retained.
    Check(pica.UnsupportedWriteCount() == 3);
    Check(pica.GetRegisterValue(0x1EF0020Cu) == 2u);
    Check(graphics.draws == 0);
}

void TestSupportedWritesAreNotCounted(bool strict) {
    wiiu::gx2::GX2Manager graphics;
    Pica200 pica(graphics);
    WiiUMemory memory(pica);
    pica.SetStrictRegisters(strict);

    pica.WriteMMIO(0x1EF00000u, 0x00804020u); // Clear colour.
    ConfigureTriangle(pica);
    pica.WriteMMIO(0x1EF00280u, 0x3F0000u);   // Uniform float 0.
    pica.WriteMMIO(0x1EF00B00u, 0u);          // Texture address 0: no texture bound.
    pica.WriteMMIO(0x1EF00B04u, 0x00080008u); // Texture dimensions.
    pica.WriteMMIO(0x1EF00B0Cu, 0u);          // Texture format.
    pica.WriteMMIO(0x1EF00C00u, 0u);          // TEV stage 0 source.
    pica.WriteMMIO(0x1EF00CFCu, 0u);          // Last TEV register.
    Check(pica.UnsupportedWriteCount() == 0 && !pica.HasUnsupportedWrite());

    pica.WriteMMIO(0x1EF0020Cu, 1u); // Valid draw.
    Check(graphics.draws == 1);
    Check(pica.UnsupportedWriteCount() == 0 && !pica.HasUnsupportedWrite());
}

void TestStrictModeThrowsBeforeStateChange() {
    wiiu::gx2::GX2Manager graphics;
    Pica200 pica(graphics);
    WiiUMemory memory(pica);
    ConfigureTriangle(pica);
    pica.SetStrictRegisters(true);

    Check(ThrowsUnsupportedAt([&] { pica.WriteMMIO(0x1EF00006u, 0xFFFFFFFFu); }, "0x1EF00006"));
    Check(pica.UnsupportedWriteCount() == 1 && pica.FirstUnsupportedAddress() == 0x1EF00006u);
    Check(pica.GetRegisterValue(0x1EF00006u) == 0);

    Check(ThrowsUnsupportedAt([&] { pica.WriteMMIO(0x1EF00010u, 0x1234u); }, "0x1EF00010"));
    Check(pica.UnsupportedWriteCount() == 2 && pica.FirstUnsupportedAddress() == 0x1EF00006u);
    Check(pica.GetRegisterValue(0x1EF00010u) == 0); // Not shadowed on throw.

    Check(ThrowsUnsupportedAt([&] { pica.WriteMMIO(0x1EF0020Cu, 2u); }, "0x1EF0020C"));
    Check(pica.UnsupportedWriteCount() == 3);
    Check(pica.GetRegisterValue(0x1EF0020Cu) == 0);
    Check(graphics.draws == 0);
    Check(graphics.writes == 0);
}

void TestOutsideWindowAndAudioAreNotCounted() {
    wiiu::gx2::GX2Manager graphics;
    Pica200 pica(graphics);
    pica.SetStrictRegisters(true);

    pica.WriteMMIO(0x1EEFFFFCu, 1u); // Below the window: not PICA.
    pica.WriteMMIO(0x1EFFFFFFu, 1u); // Exclusive end, misaligned: not PICA.
    pica.WriteMMIO(0x1F000000u, 1u); // Above the window: not PICA.
    Check(pica.UnsupportedWriteCount() == 0 && !pica.HasUnsupportedWrite());

    // Audio bridge without attached memory throws in its own path, before any counting.
    bool audio_threw = false;
    try {
        pica.WriteMMIO(0x1EC10000u, 0u);
    } catch (const std::invalid_argument&) {
        audio_threw = true;
    }
    Check(audio_threw && pica.UnsupportedWriteCount() == 0);
}

void TestCounterAccumulatesManyWrites() {
    wiiu::gx2::GX2Manager graphics;
    Pica200 pica(graphics);
    for (std::uint32_t i = 0; i < 100000u; ++i) pica.WriteMMIO(0x1EF00006u, i);
    Check(pica.UnsupportedWriteCount() == 100000u);
    Check(pica.FirstUnsupportedAddress() == 0x1EF00006u);
}
} // namespace

int main() {
    wiiu::gx2::GX2Manager graphics;
    wiiu::Pica200 pica(graphics);
    unsigned expected_writes = 0;
    for (unsigned channel = 0; channel < 3; ++channel) {
        for (std::uint32_t intensity = 0; intensity < 256; ++intensity) {
            // The upper byte must not influence the RGB register or GX2 alpha.
            const auto value = 0xAB000000u | (intensity << ((2u - channel) * 8u));
            pica.WriteMMIO(0x1EF00000u, value);
            Check(graphics.writes == ++expected_writes);
            for (unsigned component = 0; component < 3; ++component) {
                const float expected = component == channel ?
                    static_cast<float>(intensity) / 255.0f : 0.0f;
                Check(std::abs(graphics.color[component] - expected) < 0.000001f);
            }
            Check(graphics.color[3] == 1.0f);
        }
    }
    pica.WriteMMIO(0x1EF00000u, 0x00123456u);
    Check(graphics.writes == ++expected_writes);
    Check(std::abs(graphics.color[0] - 18.0f / 255.0f) < 0.000001f);
    Check(std::abs(graphics.color[1] - 52.0f / 255.0f) < 0.000001f);
    Check(std::abs(graphics.color[2] - 86.0f / 255.0f) < 0.000001f);
    const auto before = graphics.color;
    for (const auto address : {0x1EEFFFFFu, 0x1EF00004u, 0x1EFFFFFEu,
                               0x1EFFFFFFu, 0x1F000000u}) {
        pica.WriteMMIO(address, 0xFFFFFFFFu);
        Check(graphics.writes == expected_writes && graphics.color == before);
    }
    std::cout << "PICA200 RGB conversion passed (769 colors and ignored-register checks)\n";

    TestDefaultModeCountsWithoutChangingBehaviour();
    TestSupportedWritesAreNotCounted(false);
    TestSupportedWritesAreNotCounted(true);
    TestStrictModeThrowsBeforeStateChange();
    TestOutsideWindowAndAudioAreNotCounted();
    TestCounterAccumulatesManyWrites();
    std::cout << "PICA200 unsupported-write reporting passed (6 checks)\n";
}
