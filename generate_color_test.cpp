#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>

int main() {
    // ARM modified immediates encode imm8 rotated right by twice rotate_imm.
    constexpr std::array<std::uint32_t, 5> instructions{
        0xE3A0041Eu, // MOV R0, #0x1E000000
        0xE380060Fu, // ORR R0, R0, #0x00F00000 -> MVP MMIO address 0x1EF00000
        0xE3A01CFFu, // MOV R1, #0x0000FF00 -> green in 0x00RRGGBB
        0xE5801000u, // STR R1, [R0]
        0xEAFFFFFEu, // B .
    };
    std::array<std::uint8_t, instructions.size() * 4> bytes{};
    for (std::size_t i = 0; i < instructions.size(); ++i) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            bytes[i * 4 + byte] = static_cast<std::uint8_t>(instructions[i] >> (byte * 8));
        }
    }
    try {
        std::ofstream output;
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.open("color_test.code", std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.close();
        std::cout << "Generated color_test.code (20 bytes): green MMIO write and infinite loop\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "generate_color_test: " << error.what() << '\n';
        return 1;
    }
}
