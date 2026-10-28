#include "branch_translator.hpp"
#include "cfg_builder.hpp"
#include "cpp_emitter.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

void Check(bool condition) {
    if (!condition) {
        throw std::runtime_error("Branch translation regression");
    }
}

} // namespace

// Pass the output path for a generated source fixture as the single argument.
int main(int argc, char** argv) {
    Check(argc == 2);
    const arm11::BranchTranslator translator;
    const auto translate = [&translator](std::uint32_t opcode, std::uint32_t pc) {
        return translator.TranslateInstruction({opcode, 0}, pc);
    };

    Check(translate(0xEA000000u, 0x08000000u) ==
          "cpu.r[15] = 0x8000008; return;");
    Check(translate(0xEA000001u, 0x08000000u) ==
          "cpu.r[15] = 0x800000C; return;");
    Check(translate(0xEAFFFFFEu, 0x08000000u) ==
          "cpu.r[15] = 0x8000000; return;");
    Check(translate(0xEAFFFFFCu, 0x08000000u) ==
          "cpu.r[15] = 0x7FFFFF8; return;");
    Check(translate(0xEA800000u, 0x08000000u) ==
          "cpu.r[15] = 0x6000008; return;");
    Check(translate(0xEA7FFFFFu, 0x08000000u) ==
          "cpu.r[15] = 0xA000004; return;");
    Check(translate(0xEA000000u, 0xFFFFFFFCu) ==
          "cpu.r[15] = 0x4; return;");
    Check(translate(0xE12FFF1Eu, 0x08000000u) ==
          "cpu.BranchExchange(cpu.r[14]); return;");
    Check(translate(0xE12FFF10u, 0x08000000u) ==
          "cpu.BranchExchange(cpu.r[0]); return;");
    Check(translate(0x1A000000u, 0x08000000u) ==
          "cpu.r[15] = 0x8000008; return;");
    Check(translate(0x112FFF1Eu, 0x08000000u) ==
          "cpu.BranchExchange(cpu.r[14]); return;");
    Check(translate(0xEB000001u, 0x08000000u) ==
          "cpu.r[14] = 0x8000004;\ncpu.r[15] = 0x800000C;\nreturn;");
    Check(translate(0xEBFFFFFEu, 0x08000000u) ==
          "cpu.r[14] = 0x8000004;\ncpu.r[15] = 0x8000000;\nreturn;");
    Check(translate(0xEB800000u, 0x08000000u) ==
          "cpu.r[14] = 0x8000004;\ncpu.r[15] = 0x6000008;\nreturn;");
    Check(translate(0xEB7FFFFFu, 0x08000000u) ==
          "cpu.r[14] = 0x8000004;\ncpu.r[15] = 0xA000004;\nreturn;");
    Check(translate(0xEB000000u, 0xFFFFFFFCu) ==
          "cpu.r[14] = 0x0;\ncpu.r[15] = 0x4;\nreturn;");
    for (const auto opcode : {0xEF000000u, 0xE0810002u}) {
        Check(translate(opcode, 0x08000000u) == "// Unhandled Branch");
    }

    const arm11::CFGBuilder builder;
    const auto blocks = builder.BuildBlocks({
        {0xE1A00000u, 0x08000000u},
        {0xE1A01001u, 0x08000004u},
        {0xEAFFFFFEu, 0x08000008u},
        {0xE12FFF1Eu, 0x0800000Cu},
        {0xE1A02002u, 0x08000010u},
    });
    Check(blocks.size() == 4);
    Check(blocks.at(0x08000000u).instructions.size() == 2);
    Check(blocks.at(0x08000008u).instructions.size() == 1);
    Check(blocks.at(0x0800000Cu).instructions.size() == 1);

    const arm11::CppEmitter emitter;
    Check(emitter.EmitTranslatedFile(argv[1], blocks));
    std::ifstream file(argv[1]);
    Check(file.is_open());
    const std::string source{std::istreambuf_iterator<char>(file),
                             std::istreambuf_iterator<char>()};
    for (const auto* expected : {
             "cpu.r[15] = 0x8000008; return;",
             "cpu.BranchExchange(cpu.r[14]); return;",
             "cpu.r[15] = 0x08000008u;",
             "engine.RegisterBlock(0x08000008u, &arm11::block_08000008);"}) {
        Check(source.find(expected) != std::string::npos);
    }

    // BL's callee can start inside an earlier block; its return address must also
    // remain a registered block entry for the callee's BX LR.
    const auto calls = builder.BuildBlocks({
        {0xE1A00000u, 0x08000000u},
        {0xE1A01001u, 0x08000004u},
        {0xEBFFFFFDu, 0x08000008u}, // BL 0x08000004
        {0x0B000000u, 0x0800000Cu}, // BLEQ 0x08000014
        {0xE1A02002u, 0x08000010u},
        {0xE12FFF1Eu, 0x08000014u},
    });
    Check(calls.contains(0x08000004u));
    Check(calls.contains(0x0800000Cu));
    Check(calls.contains(0x08000014u));
    Check(calls.at(0x08000004u).instructions.back().opcode == 0xEBFFFFFDu);
    Check(emitter.EmitTranslatedFile(std::string{argv[1]} + ".calls.cpp", calls));
    std::ifstream call_file(std::string{argv[1]} + ".calls.cpp");
    Check(call_file.is_open());
    const std::string call_source{std::istreambuf_iterator<char>(call_file),
                                  std::istreambuf_iterator<char>()};
    Check(call_source.find("    if (cpu.cpsr.Z) {\n"
                           "        cpu.r[14] = 0x8000010;\n"
                           "        cpu.r[15] = 0x8000014;\n"
                           "        return;\n"
                           "    }\n"
                           "    cpu.r[15] = 0x08000010u;") != std::string::npos);
}
