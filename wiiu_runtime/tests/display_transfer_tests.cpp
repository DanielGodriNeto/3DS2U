#include "display_transfer.hpp"
#include "gsp_gpu.hpp"
#include "pica200.hpp"
#include "wiiu_memory.hpp"
#include "gx2_manager.hpp"
#include "hle_services.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using U32 = std::uint32_t;
constexpr U32 source = 0x08010000u, destination = 0x08080000u, packet = 0x08000000u;
constexpr U32 linear = 0x22u, rgb_out = 1u << 12;
constexpr U32 Dim(U32 w, U32 h) { return w | (h << 16); }
void Check(bool value, std::source_location at = std::source_location::current()) {
    if (!value) throw std::runtime_error("Display-transfer test failed at line " + std::to_string(at.line()));
}
template<class F> void Reject(F operation) {
    bool failed = false;
    try { operation(); } catch (const std::exception&) { failed = true; }
    Check(failed);
}
// Independent bit-interleave reference for the test image's 8x8 tile positions.
std::size_t Morton(U32 x, U32 y, U32 width) {
    std::size_t index = (static_cast<std::size_t>(y / 8u) * (width / 8u) + x / 8u) * 64;
    for (unsigned bit = 0; bit < 3; ++bit)
        index += ((x >> bit) & 1u) << (2u * bit) | ((y >> bit) & 1u) << (2u * bit + 1);
    return index;
}
void GX(wiiu::WiiUMemory& memory, U32 type, U32 src, U32 dst, U32 dim, U32 flags) {
    const std::array words{0x01000100u | type, src, dst, dim, dim, flags, 0u, 0u};
    for (U32 i = 0; i < words.size(); ++i) memory.Write32(packet + i * 4u, words[i]);
}
} // namespace

int main() try {
    wiiu::WiiUMemory memory(2u * 1024u * 1024u);
    wiiu::gx2::GX2Manager graphics;
    wiiu::DisplayTransfer transfer(graphics);
    wiiu::Pica200 pica(graphics);
    wiiu::GSPGPU gsp(pica, transfer);
    std::vector<std::uint8_t> rgba(16u * 16u * 4u);
    for (U32 y = 0; y < 16; ++y) for (U32 x = 0; x < 16; ++x) {
        const auto at = (y * 16u + x) * 4u;
        rgba[at] = 0xA5; rgba[at + 1] = static_cast<std::uint8_t>(x);
        rgba[at + 2] = static_cast<std::uint8_t>(y); rgba[at + 3] = static_cast<std::uint8_t>(x + y);
    }
    memory.LoadImage(source, rgba);
    memory.Write8(destination - 1, 0xCD); memory.Write8(destination + 16u * 16u * 3u, 0xEF);
    transfer.ExecuteTransfer(memory, source, destination, Dim(16,16), Dim(16,16), linear | rgb_out);
    for (U32 y = 0; y < 16; ++y) for (U32 x = 0; x < 16; ++x) {
        const auto at = destination + (y * 16u + x) * 3u;
        Check(memory.Read8(at) == x && memory.Read8(at + 1) == y && memory.Read8(at + 2) == x + y);
    }
    Check(memory.Read8(destination - 1) == 0xCD && memory.Read8(destination + 768) == 0xEF);
    // All four linear/Morton layout combinations preserve logical pixel positions.
    for (bool in_linear : {false, true}) for (bool out_linear : {false, true}) {
        auto input = rgba;
        for (U32 y = 0; y < 16; ++y) for (U32 x = 0; x < 16; ++x)
            for (U32 c = 0; c < 4; ++c)
                input[(in_linear ? y * 16u + x : Morton(x,y,16)) * 4u + c] = rgba[(y * 16u + x) * 4u + c];
        memory.LoadImage(source, input);
        const auto flags = (in_linear ? 2u : 0u) | (in_linear == out_linear ? 0x20u : 0u) | rgb_out;
        transfer.ExecuteTransfer(memory, source, destination, Dim(16,16), Dim(16,16), flags);
        for (U32 y = 0; y < 16; ++y) for (U32 x = 0; x < 16; ++x) {
            const auto offset = static_cast<U32>((out_linear ? y * 16u + x : Morton(x,y,16)) * 3u);
            Check(memory.Read8(destination + offset) == x);
            Check(memory.Read8(destination + offset + 1) == y);
        }
    }
    // RGB565 numeric words are guest little-endian; output RGBA8 bytes are ABGR.
    memory.LoadImage(source, std::array<std::uint8_t,8>{0x00,0xF8, 0xE0,0x07, 0x1F,0x00, 0xFF,0xFF});
    transfer.ExecuteTransfer(memory, source, destination, Dim(2,2), Dim(2,2), linear | (2u << 8));
    const std::array<std::uint8_t,16> expected{255,0,0,255, 255,0,255,0, 255,255,0,0, 255,255,255,255};
    for (U32 i = 0; i < expected.size(); ++i) Check(memory.Read8(destination + i) == expected[i]);
    transfer.ExecuteTransfer(memory, destination, source, Dim(2,2), Dim(2,2), linear | (2u << 12));
    const std::array<std::uint8_t,8> packed{0x00,0xF8, 0xE0,0x07, 0x1F,0x00, 0xFF,0xFF};
    for (U32 i = 0; i < packed.size(); ++i) Check(memory.Read8(source + i) == packed[i]);
    // Vertical flip and overlapping buffers use a snapshot of the original source.
    memory.LoadImage(source, rgba);
    transfer.ExecuteTransfer(memory, source, source + 1, Dim(16,16), Dim(16,16), linear | rgb_out | 1u);
    Check(memory.Read8(source + 1 + 1) == 15 && memory.Read8(source + 1 + 15u * 48u + 1) == 0);
    for (U32 flags : {linear | (3u << 8), linear | (4u << 12), linear | (1u << 24), 8u, 0x10000u}) {
        memory.Write8(destination, 0xCD);
        Reject([&] { transfer.ExecuteTransfer(memory, source, destination, Dim(16,16), Dim(16,16), flags); });
        Check(memory.Read8(destination) == 0xCD);
    }
    Reject([&] { transfer.ExecuteTransfer(memory, source, destination, 0, 0, linear); });
    Reject([&] { transfer.ExecuteTransfer(memory, source, destination, Dim(16,16), Dim(8,16), linear); });
    Reject([&] { transfer.ExecuteTransfer(memory, source, destination, Dim(15,16), Dim(15,16), rgb_out); });
    Reject([&] { transfer.ExecuteTransfer(memory, 0x081FFFF0u, destination, Dim(16,16), Dim(16,16), linear); });
    Reject([&] { transfer.ExecuteTransfer(memory, source, 0xFFFFFFF0u, Dim(16,16), Dim(16,16), linear); });
    // Native GSP DisplayTransfer is 0x03. 0x01 is accepted only in explicit MVP mode.
    memory.LoadImage(source, rgba);
    GX(memory, 3, source, destination, Dim(16,16), linear | rgb_out);
    gsp.ProcessGXCommand(memory, packet);
    Check(memory.Read8(destination + 2) == 0);
    wiiu::hle::HLEServiceManager services;
    Check(services.GSPGPU_SubmitGXCommand(memory, packet) != 0);
    services.AttachGSPGPU(gsp);
    Check(services.GSPGPU_SubmitGXCommand(memory, packet) == 0);
    Check(services.GSPGPU_SubmitGXCommand(memory, packet + 1) != 0);
    GX(memory, 1, source, destination, Dim(16,16), linear | rgb_out);
    Reject([&] { gsp.ProcessGXCommand(memory, packet); });
    gsp.ProcessGXCommand(memory, packet, wiiu::GSPGPU::GXDialect::MVPDisplayTransfer01);
    GX(memory, 3, source, destination, Dim(16,16), linear | rgb_out);
    memory.Write32(packet + 24, 1); Reject([&] { gsp.ProcessGXCommand(memory, packet); });
    Reject([&] { gsp.ProcessGXCommand(memory, packet + 1); });
    GX(memory, 7, source, destination, Dim(16,16), linear);
    Reject([&] { gsp.ProcessGXCommand(memory, packet); });
    // Native 0x01 still submits a PICA stream, not a transfer-shaped heuristic.
    constexpr U32 commands = packet + 0x100;
    memory.Write32(commands, 0x112233u); memory.Write32(commands + 4, 0x000F0000u);
    memory.Write32(commands + 8, 0); memory.Write32(commands + 12, 0x000F0010u);
    const std::array<U32,8> list{0x01000101u, commands, 16, 0, 0, 0, 0, 0};
    for (U32 i = 0; i < list.size(); ++i) memory.Write32(packet + i * 4u, list[i]);
    gsp.ProcessGXCommand(memory, packet);
    Check(pica.GetRegisterValue(wiiu::Pica200::kClearColorRegister) == 0x112233u);
    // Independent top/bottom snapshots, including the native sideways LCD layout.
    for (bool top : {true, false}) {
        const U32 w = top ? 400u : 320u;
        std::vector<std::uint8_t> image(static_cast<std::size_t>(w) * 240u * 4u);
        for (std::size_t i = 0; i < image.size(); i += 4) {
            image[i] = 255; image[i + 1] = top ? 0x11 : 0x22;
            image[i + 2] = top ? 0x33 : 0x44; image[i + 3] = top ? 0x55 : 0x66;
        }
        memory.LoadImage(source, image);
        GX(memory, 3, source, destination, Dim(w,240), linear | rgb_out);
        gsp.ProcessGXCommand(memory, packet);
        const auto& snapshot = graphics.display_images[top ? 0u : 1u];
        Check(snapshot.size() == image.size() && snapshot[0] == image[3] && snapshot[2] == image[1]);
        Check(memory.ConsumeFrameRequest());
        memory.Write8(destination, 0xEE); Check(snapshot[2] == image[1]);
    }
    const auto bottom_snapshot = graphics.display_images[1];
    std::vector<std::uint8_t> rotated(240u * 400u * 4u, 0);
    // Physical (239,0) is logical top-left; physical (0,399) is bottom-right.
    rotated[239u * 4u + 3] = 0xAB;
    rotated[(399u * 240u) * 4u + 1] = 0xCD;
    memory.LoadImage(source, rotated);
    transfer.ExecuteTransfer(memory, source, destination, Dim(240,400), Dim(240,400), linear);
    Check(graphics.display_images[0][0] == 0xAB);
    Check(graphics.display_images[0][(399u + 239u * 400u) * 4u + 2] == 0xCD);
    Check(graphics.display_images[1] == bottom_snapshot);
    transfer.SetFramebuffer(wiiu::DisplayTransfer::Screen::Top, destination + 0x1000);
    const auto updates = graphics.display_updates[0];
    transfer.ExecuteTransfer(memory, source, destination, Dim(240,400), Dim(240,400), linear);
    Check(graphics.display_updates[0] == updates);
    std::cout << "Display conversion, Morton layouts, bounds, GX packets and dual-screen routing passed.\n";
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
