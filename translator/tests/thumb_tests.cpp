#include "thumb_decoder.hpp"
#include "branch_translator.hpp"
#include "cpp_emitter.hpp"

#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool v) { if (!v) throw std::runtime_error("T16 regression"); }
}

int main(int argc, char** argv) {
    Check(argc == 3);
    const arm11::ThumbTranslator thumb;
    const std::array<std::uint8_t, 4> bytes{1, 0x20, 0xFE, 0xE7};
    const auto decoded = thumb.DecodeBuffer(bytes);
    Check(decoded.size() == 2 && decoded[1].address == 0x08000002u);
    Check(decoded[0].Width() == 2 && decoded[0].state == arm11::InstructionSet::Thumb);
    unsigned rejected = 0;
    try { (void)thumb.DecodeBuffer(std::span{bytes}.first(3)); } catch (const std::invalid_argument&) { ++rejected; }
    try { (void)thumb.DecodeBuffer(bytes, 1); } catch (const std::invalid_argument&) { ++rejected; }
    try { (void)thumb.DecodeBuffer(bytes, 0xFFFFFFFEu); } catch (const std::out_of_range&) { ++rejected; }
    const std::array<std::uint8_t, 2> t32{0, 0xF0};
    try { (void)thumb.DecodeBuffer(t32); } catch (const std::invalid_argument&) { ++rejected; }
    Check(rejected == 4);
    Check(arm11::ThumbTranslator::BranchTarget(0xE7FE, 0x1000) == 0x1000u);
    Check(arm11::ThumbTranslator::BranchTarget(0xD080, 0x1000) == 0xF04u);
    Check(arm11::ThumbTranslator::BranchTarget(0xE400, 0x1000) == 0x804u);

    const arm11::CFGBuilder builder;
    const auto dual = builder.BuildBlocks({{0xEAFFFFFEu, 0x1000u},
        {0xE7FEu, 0x1000u, arm11::InstructionSet::Thumb}});
    Check(dual.contains(0x1000) && dual.contains(0x1001));
    Check(dual.at(0x1000).byte_size == 4 && dual.at(0x1001).byte_size == 2);
    const auto blocks = builder.BuildBlocks({
        {0xBD03u, 0x08000000u, arm11::InstructionSet::Thumb},
        {0x2001u, 0x08000002u, arm11::InstructionSet::Thumb},
        {0xE7FEu, 0x08000004u, arm11::InstructionSet::Thumb}});
    Check(blocks.size() == 3 && blocks.at(0x08000001u).has_indirect_successor);
    Check(arm11::CppEmitter{}.EmitTranslatedFile(argv[2], blocks));

    std::ofstream out(argv[1]);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << "#include \"arm11_types.hpp\"\n#include <array>\n#include <bit>\n"
           "#include <limits>\n#include <stdexcept>\n#include <iostream>\n";
    unsigned index = 0;
    for (unsigned op = 0; op < 4; ++op) {
        for (const auto imm : {0u, 1u, 127u, 255u}) {
            out << "void op_" << index++ << "(arm11::CPUContext& cpu) {\n"
                << thumb.TranslateInstruction(static_cast<std::uint16_t>(0x2000u | (op << 11) | imm), 0x1000) << "\n}\n";
        }
    }
    for (unsigned op = 0; op < 16; ++op) {
        out << "void op_" << index++ << "(arm11::CPUContext& cpu) {\n"
            << thumb.TranslateInstruction(static_cast<std::uint16_t>(0x4008u | (op << 6)), 0x1000) << "\n}\n";
    }
    for (const auto& [name, raw] : std::array<std::pair<const char*, std::uint16_t>, 4>{
             { {"store", std::uint16_t{0x6088}}, {"load", std::uint16_t{0x688A}},
               {"push", std::uint16_t{0xB503}}, {"pop", std::uint16_t{0xBD03}} }}) {
        out << "void " << name << "(arm11::CPUContext& cpu, arm11::Memory& memory) {\n"
            << thumb.TranslateInstruction(raw, 0x1000) << "\n}\n";
    }
    out << "void thumb_bx(arm11::CPUContext& cpu) {\n" << thumb.TranslateInstruction(0x4708, 0x1000) << "\n}\n"
           "void thumb_blx(arm11::CPUContext& cpu) {\n" << thumb.TranslateInstruction(0x4788, 0x1000) << "\n}\n";
    const arm11::BranchTranslator arm;
    out << "void arm_bx(arm11::CPUContext& cpu) {\n" << arm.TranslateInstruction({0xE12FFF11u, 0}, 0x1000) << "\n}\n"
           "void arm_blx(arm11::CPUContext& cpu) {\n" << arm.TranslateInstruction({0xFA000002u, 0}, 0x1000) << "\n}\n";
    out << "struct Case { void(*run)(arm11::CPUContext&); unsigned kind, imm; };\nconst Case cases[] = {\n";
    index = 0;
    for (unsigned op = 0; op < 4; ++op) {
        for (const auto imm : {0u, 1u, 127u, 255u}) out << "{op_" << index++ << ", " << op + 16 << ", " << imm << "},\n";
    }
    for (unsigned op = 0; op < 16; ++op) out << "{op_" << index++ << ", " << op << ", 0},\n";
    out << R"cpp(};
void Check(bool v) { if (!v) throw std::runtime_error("T16 execution mismatch"); }
void Test(const Case& test, std::uint32_t a, std::uint32_t b, unsigned flags) {
    arm11::CPUContext cpu;
    for (unsigned r = 0; r < 16; ++r) cpu.r[r] = 0xCAFE0000u + r;
    cpu.r[0] = a; cpu.r[1] = b;
    cpu.cpsr = {(flags & 1u) != 0, (flags & 2u) != 0, (flags & 4u) != 0, (flags & 8u) != 0};
    auto expected = cpu;
    std::uint32_t result = a;
    bool write = true;
    const auto arithmetic = [&](std::uint32_t lhs, std::uint32_t rhs, bool sub, unsigned carry) {
        const std::uint64_t wide = std::uint64_t{lhs} + (sub ? std::uint64_t{rhs ^ 0xFFFFFFFFu} : rhs) + carry;
        result = static_cast<std::uint32_t>(wide);
        const auto sl = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(lhs));
        const auto sr = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(rhs));
        const auto signed_result = sub ? sl - sr - (1 - carry) : sl + sr + carry;
        expected.cpsr.C = (wide >> 32) != 0;
        expected.cpsr.V = signed_result < std::numeric_limits<std::int32_t>::min() ||
                          signed_result > std::numeric_limits<std::int32_t>::max();
    };
    switch (test.kind) {
    case 16: result = test.imm; break;
    case 17: arithmetic(a, test.imm, true, 1); write = false; break;
    case 18: arithmetic(a, test.imm, false, 0); break;
    case 19: arithmetic(a, test.imm, true, 1); break;
    case 0: result = a & b; break;
    case 1: result = a ^ b; break;
    case 2: case 3: case 4: case 7:
        // Bit-at-a-time reference is independent of emitted shift fast paths.
        for (unsigned i = 0; i < (b & 255u); ++i) {
            if (test.kind == 2) { expected.cpsr.C = (result >> 31) != 0; result <<= 1; }
            else {
                const bool sign = (result >> 31) != 0;
                expected.cpsr.C = (result & 1u) != 0;
                result >>= 1;
                if ((test.kind == 4 && sign) || (test.kind == 7 && expected.cpsr.C)) result |= 0x80000000u;
            }
        }
        break;
    case 5: arithmetic(a, b, false, cpu.cpsr.C ? 1u : 0u); break;
    case 6: arithmetic(a, b, true, cpu.cpsr.C ? 1u : 0u); break;
    case 8: result = a & b; write = false; break;
    case 9: arithmetic(0, b, true, 1); break;
    case 10: arithmetic(a, b, true, 1); write = false; break;
    case 11: arithmetic(a, b, false, 0); write = false; break;
    case 12: result = a | b; break;
    case 13: result = a * b; break;
    case 14: result = a & ~b; break;
    case 15: result = ~b; break;
    default: throw std::runtime_error("Bad test kind");
    }
    expected.cpsr.N = (result >> 31) != 0;
    expected.cpsr.Z = result == 0;
    if (write) expected.r[0] = result;
    test.run(cpu);
    for (unsigned r = 0; r < 16; ++r) Check(cpu.r[r] == expected.r[r]);
    Check(cpu.cpsr.N == expected.cpsr.N && cpu.cpsr.Z == expected.cpsr.Z &&
          cpu.cpsr.C == expected.cpsr.C && cpu.cpsr.V == expected.cpsr.V);
}
int main() {
    constexpr std::array<std::uint32_t, 11> values{0, 1, 31, 32, 33, 255, 256,
        0x80000000u, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x80000001u};
    unsigned count = 0;
    for (const auto& test : cases) for (unsigned flags = 0; flags < 16; ++flags) {
        for (const auto a : values) for (const auto b : values) { Test(test, a, b, flags); ++count; }
        std::uint32_t seed = 12345;
        for (unsigned n = 0; n < 512; ++n) {
            const auto a = seed = seed * 1664525u + 1013904223u;
            const auto b = seed = seed * 1664525u + 1013904223u;
            Test(test, a, b, flags); ++count;
        }
    }
    arm11::CPUContext cpu;
    arm11::Memory memory(4096, 0x08000000u);
    cpu.r[0] = 0x12345678u; cpu.r[1] = 0x08000100u;
    store(cpu, memory); load(cpu, memory); Check(cpu.r[2] == 0x12345678u);
    cpu.r[13] = 0x08000800u; cpu.r[14] = 0x08000041u;
    push(cpu, memory); cpu.r[0] = cpu.r[1] = 0; pop(cpu, memory);
    Check(cpu.r[0] == 0x12345678u && cpu.r[1] == 0x08000100u);
    Check(cpu.r[13] == 0x08000800u && cpu.r[15] == 0x08000040u && cpu.thumb);
    cpu.r[1] = 0x08000101u; arm_bx(cpu); Check(cpu.thumb && cpu.r[15] == 0x08000100u);
    cpu.r[1] = 0x08000200u; thumb_bx(cpu); Check(!cpu.thumb && cpu.r[15] == 0x08000200u);
    thumb_blx(cpu); Check(!cpu.thumb && cpu.r[14] == 0x1003u);
    arm_blx(cpu); Check(cpu.thumb && cpu.r[15] == 0x1010u && cpu.r[14] == 0x1004u);
    std::cout << "T16 passed " << count << " ALU/flag cases, memory, stack and interworking checks\n";
}
)cpp";
    out.close();
    std::cout << "T16 fetch, branch targets, dual-mode CFG and emission checks passed\n";
}
