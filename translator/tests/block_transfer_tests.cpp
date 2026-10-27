#include "mem_translator.hpp"
#include "cpp_emitter.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
void Check(bool value) {
    if (!value) throw std::runtime_error("Block transfer regression");
}
}

// Generate executable tests from actual LDM/STM bodies, plus a CFG fixture.
int main(int argc, char** argv) {
    Check(argc == 3);
    std::ofstream output(argv[1]);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << "#include \"arm11_types.hpp\"\n#include <array>\n#include <bit>\n"
              "#include <iostream>\n#include <stdexcept>\n";
    const arm11::MemoryTranslator translator;
    std::vector<std::uint32_t> opcodes;
    for (unsigned mode = 0; mode < 16; ++mode) {
        const unsigned p = (mode >> 3) & 1u;
        const unsigned u = (mode >> 2) & 1u;
        const unsigned w = (mode >> 1) & 1u;
        const unsigned l = mode & 1u;
        for (const auto list : {0x0001u, 0x00A5u, 0x400Fu, 0x800Fu, 0xFFFFu}) {
            // Writeback LDM with Rn in the list is architecturally unpredictable.
            if (l != 0 && w != 0 && (list & (1u << 13)) != 0) continue;
            const auto raw = 0xE80D0000u | (p << 24) | (u << 23) |
                             (w << 21) | (l << 20) | list;
            output << "void instruction_" << opcodes.size()
                   << "(arm11::CPUContext& cpu, arm11::Memory& memory) {\n"
                   << translator.TranslateInstruction({raw, 0}) << "\n}\n";
            opcodes.push_back(raw);
        }
    }
    output << "void push(arm11::CPUContext& cpu, arm11::Memory& memory) {\n"
           << translator.TranslateInstruction({0xE92D4003u, 0}) << "\n}\n"
              "void pop(arm11::CPUContext& cpu, arm11::Memory& memory) {\n"
           << translator.TranslateInstruction({0xE8BD8003u, 0}) << "\n}\n"
              "struct Case { void (*run)(arm11::CPUContext&, arm11::Memory&); std::uint32_t raw; };\n"
              "const Case cases[] = {\n";
    for (std::size_t i = 0; i < opcodes.size(); ++i) {
        output << "{instruction_" << i << ", " << opcodes[i] << "u},\n";
    }
    output << R"cpp(};

void Check(bool value) {
    if (!value) throw std::runtime_error("LDM/STM execution mismatch");
}

int main() {
    arm11::Memory memory(4096, 0x08000000u);
    unsigned cases_run = 0;
    for (const auto& test : cases) {
        for (const auto base : {0x08000200u, 0x08000800u}) {
            arm11::CPUContext cpu;
            for (unsigned r = 0; r < 16; ++r) cpu.r[r] = 0xCAFE0000u + r;
            cpu.r[13] = base;
            cpu.cpsr = {true, false, true, false};
            auto expected = cpu;
            std::array<std::uint32_t, 33> expected_memory{};
            for (unsigned i = 0; i < expected_memory.size(); ++i) {
                const auto addr = base - 64u + i * 4u;
                expected_memory[i] = addr ^ 0xA55A5AA5u;
                memory.Write32(addr, expected_memory[i]);
            }
            const auto list = test.raw & 0xFFFFu;
            const auto bytes = static_cast<unsigned>(std::popcount(list)) * 4u;
            const bool pre = (test.raw & (1u << 24)) != 0;
            const bool up = (test.raw & (1u << 23)) != 0;
            const bool writeback = (test.raw & (1u << 21)) != 0;
            const bool load = (test.raw & (1u << 20)) != 0;
            auto addr = up ? base + (pre ? 4u : 0u)
                           : base - bytes + (pre ? 0u : 4u);
            for (unsigned r = 0; r < 16; ++r) {
                if ((list & (1u << r)) == 0) continue;
                const auto index = (addr - (base - 64u)) / 4u;
                if (load) expected.r[r] = expected_memory[index];
                else expected_memory[index] = cpu.r[r];
                addr += 4u;
            }
            if (writeback) expected.r[13] = up ? base + bytes : base - bytes;
            test.run(cpu, memory);
            for (unsigned r = 0; r < 16; ++r) Check(cpu.r[r] == expected.r[r]);
            for (unsigned i = 0; i < expected_memory.size(); ++i) {
                Check(memory.Read32(base - 64u + i * 4u) == expected_memory[i]);
            }
            Check(cpu.cpsr.N && !cpu.cpsr.Z && cpu.cpsr.C && !cpu.cpsr.V);
            ++cases_run;
        }
    }

    arm11::CPUContext cpu;
    cpu.r[0] = 0x12345678u;
    cpu.r[1] = 0xDEADBEEFu;
    cpu.r[13] = 0x08000800u;
    cpu.r[14] = 0x08000100u;
    push(cpu, memory);
    Check(cpu.r[13] == 0x080007F4u);
    cpu.r[0] = cpu.r[1] = 0;
    pop(cpu, memory);
    Check(cpu.r[0] == 0x12345678u && cpu.r[1] == 0xDEADBEEFu);
    Check(cpu.r[13] == 0x08000800u && cpu.r[15] == 0x08000100u);
    std::cout << "LDM/STM passed " << cases_run << " mode/list cases and PUSH/POP return\n";
}
)cpp";
    output.close();

    Check(translator.TranslateInstruction({0xE8BD0000u, 0}) == "// Unhandled Instruction");
    Check(translator.TranslateInstruction({0xE8FD8001u, 0}) == "// Unhandled Instruction");
    Check(!arm11::MemoryTranslator::IsLoadMultipleToPC({0xE8FD8001u, 0}));
    const arm11::CFGBuilder builder;
    const auto blocks = builder.BuildBlocks({
        {0xE92D4003u, 0x08000000u}, // PUSH {R0, R1, LR}
        {0xE8BD8003u, 0x08000004u}, // POP {R0, R1, PC}
        {0xE1A02003u, 0x08000008u},
        {0x08BD8003u, 0x0800000Cu}, // POPEQ {R0, R1, PC}
        {0xE1A04005u, 0x08000010u},
        {0xEAFFFFFEu, 0x08000014u},
    });
    Check(blocks.size() == 4);
    Check(blocks.at(0x08000000u).instructions.size() == 2);
    Check(blocks.at(0x08000000u).has_indirect_successor);
    Check(blocks.at(0x08000008u).has_indirect_successor);
    const arm11::CppEmitter emitter;
    Check(emitter.EmitTranslatedFile(argv[2], blocks));
    std::ifstream source_file(argv[2]);
    Check(source_file.is_open());
    const std::string source{std::istreambuf_iterator<char>(source_file),
                             std::istreambuf_iterator<char>()};
    const auto first = source.find("void block_08000000");
    const auto second = source.find("void block_08000008");
    Check(first != std::string::npos && second != std::string::npos);
    const auto unconditional = source.substr(first, second - first);
    Check(unconditional.find("return;") != std::string::npos);
    Check(unconditional.find("cpu.r[15] = 0x08000008u;") == std::string::npos);
    Check(source.find("    if (cpu.cpsr.Z) {\n        {") != std::string::npos);
    Check(source.find("    cpu.r[15] = 0x08000010u;") != std::string::npos);
}
