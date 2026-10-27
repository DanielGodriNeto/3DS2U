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
          "cpu.r[15] = cpu.r[14]; return;");
    Check(translate(0xE12FFF10u, 0x08000000u) ==
          "cpu.r[15] = cpu.r[0]; return;");
    for (const auto opcode : {0xEB000000u, 0x1A000000u, 0xFA000000u,
                              0x112FFF1Eu, 0xE12FFF3Eu, 0xE0810002u}) {
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
             "cpu.r[15] = cpu.r[14]; return;",
             "cpu.r[15] = 0x08000008u;",
             "engine.RegisterBlock(0x08000008u, &arm11::block_08000008);"}) {
        Check(source.find(expected) != std::string::npos);
    }
}
