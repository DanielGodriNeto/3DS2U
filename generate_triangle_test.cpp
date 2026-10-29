#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void Put32(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data.at(offset + i) = static_cast<std::uint8_t>(value >> (i * 8));
}
std::uint32_t Immediate(std::uint32_t value) {
    for (unsigned rotate = 0; rotate < 16; ++rotate) {
        const auto candidate = std::rotl(value, static_cast<int>(rotate * 2));
        if (candidate <= 255) return (rotate << 8) | candidate;
    }
    throw std::invalid_argument("Value is not an ARM modified immediate");
}
void Save(const char* path, const std::vector<std::uint8_t>& data) {
    std::ofstream out;
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.open(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    out.close();
}
}

int main() {
    try {
        constexpr std::uint32_t base = 0x08000000u, vertices = base + 0x1000u;
        std::vector<std::uint32_t> words;
        const auto mov = [&](unsigned reg, std::uint32_t value) {
            words.push_back(0xE3A00000u | (reg << 12) | Immediate(value));
        };
        const auto orr = [&](unsigned reg, std::uint32_t value) {
            words.push_back(0xE3800000u | (reg << 16) | (reg << 12) | Immediate(value));
        };
        mov(0, 0x1E000000u); orr(0, 0x00F00000u); orr(0, 0x200u);
        mov(1, base); orr(1, 0x1000u);
        words.push_back(0xE5801000u); // VBO address.
        mov(1, 28); words.push_back(0xE5801004u); // float3 + float4 stride.
        mov(1, 3); words.push_back(0xE5801008u);
        mov(1, 1); words.push_back(0xE580100Cu);
        words.push_back(0xEAFFFFFDu); // Repeat the draw STR; runtime yields per draw.

        constexpr std::array<float, 21> triangle{
            -0.7f, -0.6f, 0.5f, 1, 0, 0, 1,
             0.7f, -0.6f, 0.5f, 0, 1, 0, 1,
             0.0f,  0.7f, 0.5f, 0, 0, 1, 1,
        };
        std::vector<std::uint8_t> code(0x2000, 0);
        for (std::size_t i = 0; i < words.size(); ++i) Put32(code, i * 4, words[i]);
        for (std::size_t i = 0; i < triangle.size(); ++i) Put32(code, 0x1000 + i * 4, std::bit_cast<std::uint32_t>(triangle[i]));
        std::vector<std::uint8_t> header(0x400, 0);
        Put32(header, 0x10, base); Put32(header, 0x14, 1);
        Put32(header, 0x18, static_cast<std::uint32_t>(words.size() * 4));
        Put32(header, 0x30, vertices); Put32(header, 0x34, 1);
        Put32(header, 0x38, static_cast<std::uint32_t>(triangle.size() * 4));
        Save("triangle_test.code", code);
        Save("triangle_test.exheader", header);
        std::cout << "Generated triangle_test.code and triangle_test.exheader (3 RGB vertices)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "generate_triangle_test: " << error.what() << '\n';
        return 1;
    }
}
