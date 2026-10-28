#include "gsp_gpu.hpp"
#include "pica200.hpp"
#include "wiiu_memory.hpp"
#include "gx2_manager.hpp"
#include "float24.hpp"
#include "default_shaders.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <bit>
#include <cmath>

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

void CheckFloat24() {
    Check(wiiu::float24_to_float32(0) == 0.0f);
    Check(wiiu::float24_to_float32(0x3F0000u) == 1.0f);
    Check(wiiu::float24_to_float32(0x3F8000u) == 1.5f);
    Check(wiiu::float24_to_float32(0xBF0000u) == -1.0f);
    Check(std::bit_cast<std::uint32_t>(wiiu::float24_to_float32(0x800000u)) == 0x80000000u);
    Check(std::isinf(wiiu::float24_to_float32(0x7F0000u)));
    Check(std::isnan(wiiu::float24_to_float32(0x7F0001u)));
    Check(wiiu::float24_to_float32(0xFF3F0000u) == 1.0f);
    for (unsigned sign = 0; sign < 2; ++sign) {
        for (std::uint32_t mantissa = 1; mantissa < 65536; ++mantissa) {
            float expected = std::ldexp(static_cast<float>(mantissa), -78);
            if (sign != 0) expected = -expected;
            Check(wiiu::float24_to_float32((sign << 23) | mantissa) == expected);
        }
        for (std::uint32_t exponent = 1; exponent < 127; ++exponent) {
            for (std::uint32_t mantissa : {0u, 1u, 0x8000u, 0xFFFFu}) {
                float expected = std::ldexp(1.0f + static_cast<float>(mantissa) / 65536.0f,
                                           static_cast<int>(exponent) - 63);
                if (sign != 0) expected = -expected;
                Check(wiiu::float24_to_float32((sign << 23) | (exponent << 16) | mantissa) == expected);
            }
        }
    }
}

// Execute the encoded DOT4 clauses on the host to check matrix indexing/export.
std::array<float, 4> RunVertexProgram(const std::vector<float>& constants,
                                     const std::array<float, 4>& input) {
    const auto& words = wiiu::gx2::shaders::vertex;
    const auto address = words[2] & 0x3FFFFFu;
    Check(((words[3] >> 18) & 0x7Fu) + 1 == 16);
    Check(((words[4] >> 15) & 0x7Fu) == 3);
    std::array<float, 4> result{};
    for (unsigned group = 0; group < 4; ++group) {
        float dot = 0;
        for (unsigned slot = 0; slot < 4; ++slot) {
            const auto word0 = words[address * 2 + group * 8 + slot * 2];
            const auto word1 = words[address * 2 + group * 8 + slot * 2 + 1];
            Check((word0 & 0x1FFu) == 1 && ((word1 >> 7) & 0x7FFu) == 0x50);
            Check((word0 >> 31) == (slot == 3 ? 1u : 0u));
            const auto reg = (word0 >> 13) & 0x1FFu;
            Check(reg >= 256 && reg < 260);
            dot += input[(word0 >> 10) & 3u] * constants[(reg - 256) * 4 + ((word0 >> 23) & 3u)];
        }
        for (unsigned slot = 0; slot < 4; ++slot) {
            const auto word1 = words[address * 2 + group * 8 + slot * 2 + 1];
            if ((word1 & 0x10u) != 0) {
                Check(((word1 >> 21) & 0x7Fu) == 3);
                result[(word1 >> 29) & 3u] = dot;
            }
        }
    }
    return result;
}
} // namespace

int main() {
    CheckFloat24();
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
    Packet(commands, 0xA0, {0x3F0000u, 0x400000u, 0x404000u}, true);
    Packet(commands, 0x10, {0x12345678u});
    Packet(commands, 0x10, {0x12345678u});
    Packet(commands, 0, {0x00FF0000u}); // FINALIZE terminates before this write.
    graphics.before_draw = [&] {
        Check(pica.GetRegisterValue(0x1EF00288u) == 0x404000u);
    };
    Run(gsp, memory, commands);
    Check(graphics.draws == 1 && graphics.vertex_count == 3 && graphics.vertex_stride == 28);
    Check(graphics.color == std::array<float, 4>{0, 1, 0, 1});
    Check(pica.GetRegisterValue(0x1EF00200u) == 0x08001000u);
    Check(pica.GetRegisterValue(0x1EF00280u) == 0x3F0000u);
    Check(pica.GetRegisterValue(0x1EF00284u) == 0x400000u);
    Check(std::equal(graphics.vertex_bytes.begin(), graphics.vertex_bytes.end(), vertices.begin()));
    Check(memory.ConsumeFrameRequest() && !memory.ConsumeFrameRequest());
    Check(graphics.uniforms.size() == 384);
    for (unsigned i = 0; i < 16; ++i)
        Check(graphics.uniforms[i] == (i % 5 == 0 ? 1.0f : 0.0f));
    Check(RunVertexProgram(graphics.uniforms, {1, 2, 3, 1}) == std::array<float, 4>{1, 2, 3, 1});
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

    // Uniform writes after a trigger must not alter that draw's captured matrix.
    commands.clear();
    Packet(commands, 0xA0, {0x3F0000u, 0, 0, 0,
                          0, 0x3F0000u, 0, 0,
                          0, 0, 0x3F0000u, 0,
                          0, 0, 0, 0x3F0000u}, true);
    Packet(commands, 0x83, {1});
    Packet(commands, 0xA0, {0x400000u, 0, 0, 0x404000u}, true); // x' = 2*x + 2.5*w
    Packet(commands, 0x83, {1});
    Packet(commands, 0xA0, {0x408000u}); // Later 3.0 must not leak into either draw.
    Packet(commands, 0x21F, {0xBF0000u}); // Last of 384 scalar slots, address 0x1EF0087C.
    Run(gsp, memory, commands);
    const auto total = graphics.uniform_history.size();
    Check(total >= 2);
    const auto& first = graphics.uniform_history[total - 2];
    const auto& second = graphics.uniform_history[total - 1];
    Check(first[0] == 1.0f && second[0] == 2.0f && second[3] == 2.5f);
    Check(RunVertexProgram(second, {1, 2, 3, 1}) == std::array<float, 4>{4.5f, 2, 3, 1});
    Check(pica.GetVertexUniforms()[0] == 3.0f && pica.GetVertexUniforms()[383] == -1.0f);
    Check(graphics.uniforms[383] != -1.0f);
    for (std::uint32_t i = 0; i < wiiu::Pica200::kUniformFloatCount; ++i)
        pica.WriteMMIO(wiiu::Pica200::kUniformBase + i * 4u, 0x3F0000u + i);
    const auto all_uniforms = pica.GetVertexUniforms();
    for (std::uint32_t i = 0; i < wiiu::Pica200::kUniformFloatCount; ++i)
        Check(all_uniforms[i] == 1.0f + static_cast<float>(i) / 65536.0f);
    commands.clear();
    Packet(commands, 0x83, {1});
    Run(gsp, memory, commands);
    Check(std::equal(all_uniforms.begin(), all_uniforms.end(), graphics.uniforms.begin()));
    std::cout << "Float24 conversion, matrix shader, uniform snapshots and GSP regression tests passed.\n";
}
