#include "../include/arm11_types.hpp"
#include "../include/binary_loader.hpp"
#include "../include/decoder.hpp"
#include "../include/cfg_builder.hpp"
#include "../include/cpp_emitter.hpp"
#include <iostream>
#include <fstream>

int main() {
    // 1. Create a binary containing various ARM11 instructions
    const std::string dummy_file = "dummy_program.bin";
    std::ofstream ofs(dummy_file, std::ios::binary);
    
    uint32_t dummy_instructions[] = {
        0xE1A01002, // MOV R1, R2
        0xE0813002, // ADD R3, R1, R2
        0xEAFFFFFE  // B . (Infinite branch to itself)
    };
    ofs.write(reinterpret_cast<const char*>(dummy_instructions), sizeof(dummy_instructions));
    ofs.close();

    // 2. Load the Binary
    arm11::BinaryLoader loader;
    if (!loader.LoadFile(dummy_file)) {
        std::cerr << "[3DS2U Recompiler] Failed to load the binary file.\n";
        return 1;
    }

    // 3. Decode Instructions
    arm11::Decoder decoder;
    auto instructions = decoder.DecodeBuffer(loader.GetCodeBuffer(), 0x08000000);

    // 4. Build the Control Flow Graph (CFG)
    arm11::CFGBuilder cfg;
    auto blocks = cfg.BuildBlocks(instructions);

    // 5. Emit the Translated C++ Code to File
    arm11::CppEmitter emitter;
    const std::string output_cpp = "translated_game.cpp";
    if (emitter.EmitTranslatedFile(output_cpp, blocks)) {
        std::cout << "[3DS2U Recompiler] SUCCESS! Module 1 Pipeline completed.\n";
        std::cout << "C++ file successfully generated: " << output_cpp << "\n";
    } else {
        std::cerr << "[3DS2U Recompiler] Failed to emit C++ file.\n";
        return 1;
    }

    return 0;
}