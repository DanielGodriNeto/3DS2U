#include "alu_translator.hpp"
#include "cpp_emitter.hpp"

#include <array>
#include <fstream>
#include <stdexcept>

// Generates executable flag tests from the actual translated instruction bodies.
int main(int argc, char** argv) {
    if (argc != 3) throw std::invalid_argument("Expected test and block output paths");
    std::ofstream output(argv[1]);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << "#include \"arm11_types.hpp\"\n"
              "#include <array>\n#include <bit>\n#include <limits>\n"
              "#include <iostream>\n#include <stdexcept>\n\n";

    arm11::ALUTranslator translator;
    unsigned index = 0;
    for (const auto opcode : {4u, 2u, 13u, 10u}) {
        for (const auto s : {0u, 1u}) {
            for (const auto rd : {0u, 1u, 2u}) {
                const std::uint32_t raw = 0xE0000000u | (opcode << 21) |
                                          (s << 20) | (rd << 12) | 1u;
                output << "void instruction_" << index++ << "(arm11::CPUContext& cpu) {\n"
                       << translator.TranslateInstruction({raw, 0}) << "\n}\n";
            }
        }
    }
    output << "struct Case { void (*run)(arm11::CPUContext&); unsigned op, s, rd; };\n"
              "const Case cases[] = {\n";
    index = 0;
    for (const auto opcode : {4u, 2u, 13u, 10u}) {
        for (const auto s : {0u, 1u}) {
            for (const auto rd : {0u, 1u, 2u}) {
                output << "{instruction_" << index++ << ", " << opcode << ", " << s
                       << ", " << rd << "},\n";
            }
        }
    }
    output << R"cpp(};

void Check(const Case& test, std::uint32_t a, std::uint32_t b, unsigned flags) {
    arm11::CPUContext actual;
    for (unsigned r = 0; r < 16; ++r) actual.r[r] = 0xBAADF000u + r;
    actual.r[0] = a;
    actual.r[1] = b;
    actual.cpsr = {(flags & 1u) != 0, (flags & 2u) != 0,
                   (flags & 4u) != 0, (flags & 8u) != 0};
    auto expected = actual;
    const bool add = test.op == 4;
    const bool mov = test.op == 13;
    const std::uint64_t wide = add ? std::uint64_t{a} + b
                                  : std::uint64_t{a} + std::uint64_t{b ^ 0xFFFFFFFFu} + 1u;
    const auto result = mov ? b : static_cast<std::uint32_t>(wide);
    if (test.s != 0 || test.op == 10) {
        expected.cpsr.N = result >= 0x80000000u;
        expected.cpsr.Z = result == 0;
        if (!mov) {
            const auto sa = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(a));
            const auto sb = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(b));
            const auto signed_result = add ? sa + sb : sa - sb;
            expected.cpsr.C = (wide >> 32) != 0;
            expected.cpsr.V = signed_result > std::numeric_limits<std::int32_t>::max() ||
                              signed_result < std::numeric_limits<std::int32_t>::min();
        }
    }
    if (test.op != 10) expected.r[test.rd] = result;
    test.run(actual);
    for (unsigned r = 0; r < 16; ++r) {
        if (actual.r[r] != expected.r[r]) throw std::runtime_error("ALU register mismatch");
    }
    if (actual.cpsr.N != expected.cpsr.N || actual.cpsr.Z != expected.cpsr.Z ||
        actual.cpsr.C != expected.cpsr.C || actual.cpsr.V != expected.cpsr.V) {
        throw std::runtime_error("ALU flag mismatch");
    }
}

int main() {
    constexpr std::array<std::uint32_t, 7> values{
        0, 1, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x40000000u, 0xC0000000u};
    for (const auto& test : cases) {
        for (unsigned flags = 0; flags < 16; ++flags) {
            for (const auto a : values) {
                for (const auto b : values) Check(test, a, b, flags);
            }
            std::uint32_t seed = 12345;
            for (unsigned n = 0; n < 512; ++n) {
                const auto a = seed = seed * 1664525u + 1013904223u;
                const auto b = seed = seed * 1664525u + 1013904223u;
                Check(test, a, b, flags);
            }
        }
    }
    std::cout << "ALU execution and flags passed (215424 cases)\n";
}
)cpp";
    output.close();

    // Compile emitted blocks to verify consecutive temporaries and CPSR guards.
    const arm11::CFGBuilder builder;
    const auto cfg = builder.BuildBlocks({
        {0xE0900001u, 0x08000000u}, // ADDS R0, R0, R1
        {0xE0502001u, 0x08000004u}, // SUBS R2, R0, R1
        {0xE1500001u, 0x08000008u}, // CMP R0, R1
        {0x01B03002u, 0x0800000Cu}, // MOVSEQ R3, R2
        {0x10900001u, 0x08000010u}, // ADDSNE R0, R0, R1
        {0xEAFFFFFEu, 0x08000014u}, // B to itself
    });
    const arm11::CppEmitter emitter;
    if (!emitter.EmitTranslatedFile(argv[2], cfg)) {
        throw std::runtime_error("Cannot emit ALU fixture");
    }
}
