#include "gsp_gpu.hpp"
#include "pica200.hpp"
#include "wiiu_memory.hpp"
#include "gx2_manager.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void Check(bool condition) {
    if (!condition) throw std::runtime_error("GSP command parser test failed");
}
using Words = std::vector<std::uint32_t>;
void Packet(Words& words, std::uint32_t reg, std::initializer_list<std::uint32_t> values,
            bool consecutive = false, std::uint32_t mask = 0xF) {
    Check(values.size() > 0 && values.size() <= 256);
    const auto extra = static_cast<std::uint32_t>(values.size() - 1);
    words.push_back(*values.begin());
    words.push_back(reg | (mask << 16) | (extra << 20) | (consecutive ? 0x80000000u : 0u));
    words.insert(words.end(), values.begin() + 1, values.end());
    if ((extra & 1u) != 0) words.push_back(0xBAADF00Du);
}
constexpr std::uint32_t kCommands = 0x08000000u;
void Load(wiiu::WiiUMemory& memory, const Words& words) {
    for (std::size_t i = 0; i < words.size(); ++i)
        memory.Write32(kCommands + static_cast<std::uint32_t>(i * 4), words[i]);
}
void Run(wiiu::GSPGPU& gsp, wiiu::WiiUMemory& memory, const Words& words) {
    Load(memory, words);
    gsp.ProcessCommandBuffer(memory, kCommands, static_cast<std::uint32_t>(words.size() * 4));
}
template<class Operation> void Reject(Operation operation) {
    bool caught = false;
    try { operation(); } catch (const std::exception&) { caught = true; }
    Check(caught);
}
} // namespace

int main() {
    wiiu::gx2::GX2Manager graphics;
    wiiu::Pica200 pica(graphics);
    wiiu::WiiUMemory memory(pica);
    wiiu::GSPGPU gsp(pica);
    std::array<std::uint8_t, 168> vertices{};
    for (std::size_t i = 0; i < vertices.size(); ++i) vertices[i] = static_cast<std::uint8_t>(i);
    memory.LoadImage(0x08001000u, vertices);

    Words commands;
    Packet(commands, 0, {0x0000FF00u});
    Packet(commands, 0x80, {0x08001000u, 28, 3, 1}, true);
    // This uniform write follows the draw trigger and must precede native submission.
    Packet(commands, 0xA0, {0x3F800000u, 0x40000000u, 0x40400000u}, true);
    Packet(commands, 0x10, {0x12345678u});
    Packet(commands, 0x10, {0x12345678u});
    Packet(commands, 0, {0x00FF0000u}); // FINALIZE terminates before this write.
    graphics.before_draw = [&] {
        Check(pica.GetRegisterValue(0x1EF00288u) == 0x40400000u);
    };
    Run(gsp, memory, commands);
    Check(graphics.draws == 1 && graphics.vertex_count == 3 && graphics.vertex_stride == 28);
    Check(graphics.color == std::array<float, 4>{0, 1, 0, 1});
    Check(pica.GetRegisterValue(0x1EF00200u) == 0x08001000u);
    Check(pica.GetRegisterValue(0x1EF00280u) == 0x3F800000u);
    Check(pica.GetRegisterValue(0x1EF00284u) == 0x40000000u);
    Check(std::equal(graphics.vertex_bytes.begin(), graphics.vertex_bytes.end(), vertices.begin()));
    Check(memory.ConsumeFrameRequest() && !memory.ConsumeFrameRequest());
    graphics.before_draw = {};

    commands.clear();
    pica.WriteMMIO(0x1EF00280u, 0x11223344u);
    Packet(commands, 0xA0, {0xAABBCCDDu}, false, 5);
    Packet(commands, 0xA1, {1, 2, 3}); // FIFO: same register, last value wins.
    Packet(commands, 0, {0xFFFFFFFFu}, false, 0);
    Packet(commands, 0x83, {0}, false, 8); // Does not retrigger preserved low byte.
    Run(gsp, memory, commands);
    Check(pica.GetRegisterValue(0x1EF00280u) == 0x11BB33DDu);
    Check(pica.GetRegisterValue(0x1EF00284u) == 3);
    Check(graphics.draws == 1 && graphics.writes == 1 && !memory.ConsumeFrameRequest());
    // Exhaust all byte masks against an independent byte-wise reference.
    for (std::uint32_t mask = 0; mask < 16; ++mask) {
        pica.WriteMMIO(0x1EF00280u, 0x12345678u);
        commands.clear();
        Packet(commands, 0xA0, {0xABCDEF01u}, false, mask);
        Run(gsp, memory, commands);
        std::uint32_t expected = 0;
        for (unsigned byte = 0; byte < 4; ++byte) {
            const auto source = (mask & (1u << byte)) != 0 ? 0xABCDEF01u : 0x12345678u;
            expected |= source & (0xFFu << (byte * 8u));
        }
        Check(pica.GetRegisterValue(0x1EF00280u) == expected);
    }

    // Largest packet: 256 values, an odd extra count, and one padding word.
    commands = {0x10000u, 0x8FFF0100u};
    for (std::uint32_t i = 1; i < 256; ++i) commands.push_back(0x10000u + i);
    commands.push_back(0xBAADF00Du);
    Run(gsp, memory, commands);
    for (std::uint32_t i = 0; i < 256; ++i)
        Check(pica.GetRegisterValue(0x1EF00400u + i * 4u) == 0x10000u + i);

    commands.clear();
    Packet(commands, 0x82, {3, 1}, true);
    Packet(commands, 0x82, {6, 1}, true);
    Packet(commands, 0x82, {0});
    Run(gsp, memory, commands); // Natural end also flushes; snapshots keep both counts.
    Check(graphics.draw_counts == std::vector<std::uint32_t>{3, 3, 6});
    Check(memory.ConsumeFrameRequest());

    // A truncated later packet must not apply the earlier valid clear write.
    commands.clear();
    Packet(commands, 0, {0x00FF0000u});
    commands.push_back(0);
    commands.push_back(0x001F00A0u); // One extra parameter plus padding missing.
    Reject([&] { Run(gsp, memory, commands); });
    Check(graphics.writes == 1 && graphics.draws == 3);
    Reject([&] { Run(gsp, memory, Words{0, 0x100F0000u}); });
    Reject([&] { Run(gsp, memory, Words{1, 0x801FFFFFu, 2, 0}); });
    Reject([&] { gsp.ProcessCommandBuffer(memory, kCommands + 4, 8); });
    Reject([&] { gsp.ProcessCommandBuffer(memory, kCommands, 4); });
    Reject([&] { gsp.ProcessCommandBuffer(memory, 0xFFFFFFF8u, 16); });
    Reject([&] { gsp.ProcessCommandBuffer(memory, 0x0800FFF8u, 16); });

    commands.clear();
    Packet(commands, 0x80, {0x0800FFF0u, 28, 3, 1}, true);
    Reject([&] { Run(gsp, memory, commands); });
    Check(graphics.draws == 3 && !memory.ConsumeFrameRequest());
    commands.clear();
    Packet(commands, 0x80, {0x08001000u, 28, 3, 1}, true);
    Run(gsp, memory, commands); // Failed list must not leave batching active.
    Check(graphics.draws == 4 && memory.ConsumeFrameRequest());
    gsp.ProcessCommandBuffer(memory, 0, 0);
    std::cout << "GSP packets, masks, batches, deferred draws and malformed-buffer tests passed.\n";
}
