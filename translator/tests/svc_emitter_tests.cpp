#include "cfg_builder.hpp"
#include "cpp_emitter.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    arm11::CFGBuilder builder;
    arm11::CppEmitter emitter;
    const auto emit = [&](const char* file, const std::vector<arm11::Instruction>& instructions) {
        const auto blocks = builder.BuildBlocks(instructions);
        if (blocks.size() != 2 || blocks.at(arm11::DispatchKey({instructions[0].address, instructions[0].state})).instructions.size() != 1 ||
            !emitter.EmitTranslatedFile(file, blocks)) throw std::runtime_error("SVC CFG/emission failed");
    };
    emit("build/svc_arm.cpp", {{0xEF000001u, 0x08000000u}, {0xEF000009u, 0x08000004u}});
    emit("build/svc_thumb.cpp", {{0xDF01u, 0x08000010u, arm11::InstructionSet::Thumb},
                                  {0xDF09u, 0x08000012u, arm11::InstructionSet::Thumb}});
    emit("build/svc_conditional.cpp", {{0x1F000001u, 0x08000020u}, {0xEAFFFFFEu, 0x08000024u}});
    std::cout << "ARM/Thumb/conditional SVC emission and CFG boundaries passed.\n";
}
