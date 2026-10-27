#include "mem_translator.hpp"
#include "cpp_emitter.hpp"

#include <fstream>
#include <stdexcept>
#include <vector>

// Generate executable tests from the translator's actual memory instruction bodies.
int main(int argc, char** argv) {
    if (argc != 3) throw std::invalid_argument("Expected execution and block output paths");
    std::ofstream output(argv[1]);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << R"cpp(#include "arm11_types.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

struct TestMemory {
    arm11::CPUContext& cpu;
    std::uint32_t address{}, stored{}, observed_base{};
    unsigned reads{}, writes{};
    static constexpr std::uint32_t loaded = 0x12345678u;
    std::uint32_t Read32(std::uint32_t addr) {
        address = addr; observed_base = cpu.r[0]; ++reads; return loaded;
    }
    void Write32(std::uint32_t addr, std::uint32_t value) {
        address = addr; stored = value; observed_base = cpu.r[0]; ++writes;
    }
};
)cpp";
    arm11::MemoryTranslator translator;
    std::vector<std::uint32_t> opcodes;
    for (unsigned p = 0; p < 2; ++p) {
        for (unsigned u = 0; u < 2; ++u) {
            for (unsigned w = 0; w < 2; ++w) {
                for (unsigned l = 0; l < 2; ++l) {
                    for (const auto offset : {0u, 4u, 4095u}) {
                        for (const auto rd : {0u, 2u}) {
                            // LDR writeback with Rn==Rd is architecturally unpredictable.
                            if (l != 0 && rd == 0 && (p == 0 || w != 0)) continue;
                            const auto raw = 0xE4000000u | (p << 24) | (u << 23) |
                                             (w << 21) | (l << 20) | (rd << 12) | offset;
                            output << "void instruction_" << opcodes.size()
                                   << "(arm11::CPUContext& cpu, TestMemory& memory) {\n"
                                   << translator.TranslateInstruction({raw, 0}) << "\n}\n";
                            opcodes.push_back(raw);
                        }
                    }
                }
            }
        }
    }
    for (const auto raw : {0xE7902001u, 0xE5D02000u, 0xEAFFFFFEu}) {
        if (translator.TranslateInstruction({raw, 0}) != "// Unhandled Instruction") {
            throw std::runtime_error("Unsupported memory encoding accepted");
        }
    }
    output << "struct Case { void (*run)(arm11::CPUContext&, TestMemory&); std::uint32_t raw; };\n"
              "const Case cases[] = {\n";
    for (std::size_t i = 0; i < opcodes.size(); ++i) {
        output << "{instruction_" << i << ", " << opcodes[i] << "u},\n";
    }
    output << R"cpp(};

int main() {
    unsigned count = 0;
    for (const auto& test : cases) {
        for (const std::uint32_t base : {0u, 0x08000000u, 0xFFFFFFFFu, 0x1000u}) {
            arm11::CPUContext cpu;
            for (unsigned r = 0; r < 16; ++r) cpu.r[r] = 0xCAFE0000u + r;
            cpu.r[0] = base;
            cpu.cpsr = {true, false, true, false};
            auto expected = cpu;
            const bool pre = (test.raw & (1u << 24)) != 0;
            const bool up = (test.raw & (1u << 23)) != 0;
            const bool writeback = (test.raw & (1u << 21)) != 0;
            const bool load = (test.raw & (1u << 20)) != 0;
            const auto rd = (test.raw >> 12) & 15u;
            const auto offset = static_cast<std::int64_t>(test.raw & 4095u);
            const auto adjusted = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(base) + (up ? offset : -offset));
            const auto source = cpu.r[rd];
            if (pre && writeback) expected.r[0] = adjusted;
            if (load) expected.r[rd] = TestMemory::loaded;
            if (!pre) expected.r[0] = adjusted;
            TestMemory memory{cpu};
            test.run(cpu, memory);
            if (memory.address != (pre ? adjusted : base) ||
                memory.observed_base != (pre && writeback ? adjusted : base) ||
                memory.reads != (load ? 1u : 0u) ||
                memory.writes != (load ? 0u : 1u) ||
                (!load && memory.stored != source)) {
                throw std::runtime_error("Addressing or writeback order mismatch");
            }
            for (unsigned r = 0; r < 16; ++r) {
                if (cpu.r[r] != expected.r[r]) throw std::runtime_error("Register mismatch");
            }
            if (!cpu.cpsr.N || cpu.cpsr.Z || !cpu.cpsr.C || cpu.cpsr.V) {
                throw std::runtime_error("Memory instruction changed CPSR");
            }
            ++count;
        }
    }
    std::cout << "Memory addressing execution passed (" << count << " cases)\n";
}
)cpp";
    output.close();

    const arm11::CFGBuilder builder;
    const auto cfg = builder.BuildBlocks({
        {0xE5B02010u, 0x08000000u}, // LDR R2, [R0, #0x10]!
        {0xE4002004u, 0x08000004u}, // STR R2, [R0], #-4
        {0x05903000u, 0x08000008u}, // LDREQ R3, [R0]
        {0xEAFFFFFEu, 0x0800000Cu},
    });
    const arm11::CppEmitter emitter;
    if (!emitter.EmitTranslatedFile(argv[2], cfg)) {
        throw std::runtime_error("Cannot emit memory fixture");
    }
}
