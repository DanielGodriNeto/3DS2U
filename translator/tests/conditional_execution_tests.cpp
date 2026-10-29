#include "arm11_types.hpp"
#include "cond_evaluator.hpp"
#include "cpp_emitter.hpp"

#include <array>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

void Check(bool condition) {
    if (!condition) throw std::runtime_error("Conditional execution regression");
}

constexpr bool CheckFlagHelpers() {
    arm11::CPUContext cpu;
    cpu.UpdateNZCV(0x80000000u, true, true);
    if (!cpu.cpsr.N || cpu.cpsr.Z || !cpu.cpsr.C || !cpu.cpsr.V) return false;
    cpu.UpdateNZ(0);
    if (cpu.cpsr.N || !cpu.cpsr.Z || !cpu.cpsr.C || !cpu.cpsr.V) return false;
    cpu.SetFlag(arm11::CPUContext::C | arm11::CPUContext::V, false);
    return !cpu.cpsr.C && !cpu.cpsr.V && cpu.Flag(arm11::CPUContext::Z);
}

static_assert(CheckFlagHelpers());

} // namespace

// Pass the output path for a generated conditional-block fixture.
int main(int argc, char** argv) {
    Check(argc == 2);
    const std::array<std::string, 16> expected{
        "cpu.cpsr.Z", "!cpu.cpsr.Z", "cpu.cpsr.C", "!cpu.cpsr.C",
        "cpu.cpsr.N", "!cpu.cpsr.N", "cpu.cpsr.V", "!cpu.cpsr.V",
        "(cpu.cpsr.C && !cpu.cpsr.Z)", "(!cpu.cpsr.C || cpu.cpsr.Z)",
        "(cpu.cpsr.N == cpu.cpsr.V)", "(cpu.cpsr.N != cpu.cpsr.V)",
        "(!cpu.cpsr.Z && (cpu.cpsr.N == cpu.cpsr.V))",
        "(cpu.cpsr.Z || (cpu.cpsr.N != cpu.cpsr.V))", "", "false",
    };
    for (std::uint8_t cond = 0; cond < expected.size(); ++cond) {
        Check(arm11::ConditionEvaluator::GetConditionString(cond) == expected[cond]);
    }
    bool rejected = false;
    try {
        (void)arm11::ConditionEvaluator::GetConditionString(16);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    Check(rejected);

    const arm11::CFGBuilder builder;
    const auto blocks = builder.BuildBlocks({
        {0x01A00001u, 0x08000000u}, // MOVEQ R0, R1
        {0x10823004u, 0x08000004u}, // ADDNE R3, R2, R4
        {0x0AFFFFFEu, 0x08000008u}, // BEQ to itself
        {0x112FFF1Eu, 0x0800000Cu}, // BXNE LR
        {0xE1A05006u, 0x08000010u}, // MOV R5, R6
        {0xEAFFFFFEu, 0x08000014u}, // B to itself
    });
    Check(blocks.contains(0x08000008u));
    Check(blocks.at(0x08000000u).instructions.size() == 2);
    Check(blocks.at(0x08000008u).instructions.size() == 1);

    const arm11::CppEmitter emitter;
    Check(emitter.EmitTranslatedFile(argv[1], blocks));
    std::ifstream file(argv[1]);
    Check(file.is_open());
    const std::string source{std::istreambuf_iterator<char>(file),
                             std::istreambuf_iterator<char>()};
    for (const auto* fragment : {
             "    if (cpu.cpsr.Z) {\n        cpu.r[0] = cpu.r[1];\n    }",
             "    if (!cpu.cpsr.Z) {\n        cpu.r[3] = cpu.r[2] + cpu.r[4];\n    }",
             "    if (cpu.cpsr.Z) {\n        cpu.r[15] = 0x8000008; return;\n    }\n"
             "    cpu.r[15] = 0x0800000Cu;",
             "    if (!cpu.cpsr.Z) {\n        cpu.BranchExchange(cpu.r[14]); return;\n    }\n"
             "    cpu.r[15] = 0x08000010u;",
             "    cpu.r[5] = cpu.r[6];\n    cpu.r[15] = 0x08000014u;"}) {
        Check(source.find(fragment) != std::string::npos);
    }
}
