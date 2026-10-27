#include "binary_loader.hpp"
#include "decoder.hpp"
#include "cfg_builder.hpp"
#include "cpp_emitter.hpp"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: 3ds2u_translator <input_binary.code> <output_source.cpp>\n";
        return 1;
    }

    const std::string input_path = argv[1];
    const std::string output_path = argv[2];
    std::cout << "[3DS2U] Starting translation\n"
              << "[3DS2U] Input:  \"" << input_path << "\"\n"
              << "[3DS2U] Output: \"" << output_path << "\"\n";

    try {
        arm11::BinaryLoader loader;
        if (!loader.LoadFile(input_path)) {
            std::cerr << "[3DS2U] Translation failed: cannot load \"" << input_path << "\"\n";
            return 1;
        }

        arm11::Decoder decoder;
        const auto instructions = decoder.DecodeBuffer(loader.GetCodeBuffer(), 0x08000000u);
        arm11::CFGBuilder builder;
        const auto blocks = builder.BuildBlocks(instructions);
        arm11::CppEmitter emitter;
        if (!emitter.EmitTranslatedFile(output_path, blocks)) {
            std::cerr << "[3DS2U] Translation failed: cannot emit \"" << output_path << "\"\n";
            return 1;
        }

        std::cout << "[3DS2U] Translation complete: \"" << input_path << "\" -> \""
                  << output_path << "\"\n"
                  << "[3DS2U] Decoded " << instructions.size() << " instructions into "
                  << blocks.size() << " basic blocks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[3DS2U] Translation failed: " << error.what() << '\n';
        return 1;
    }
}
