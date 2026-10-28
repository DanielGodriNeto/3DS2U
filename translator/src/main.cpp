#include "binary_loader.hpp"
#include "decoder.hpp"
#include "cfg_builder.hpp"
#include "cpp_emitter.hpp"

#include <exception>
#include <iomanip>
#include <stdexcept>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3 || (argc != 3 &&
        (argc != 5 || std::string{argv[3]} != "--exheader"))) {
        std::cerr << "Usage: 3ds2u_translator <input_binary.code> <output_source.cpp>\n";
        std::cerr << "       Add --exheader <decrypted.exheader> for ExeFS section mapping\n";
        return 1;
    }

    const std::string input_path = argv[1];
    const std::string output_path = argv[2];
    const std::string exheader_path = argc == 5 ? argv[4] : "";
    std::cout << "[3DS2U] Starting translation\n"
              << "[3DS2U] Input:  \"" << input_path << "\"\n"
              << "[3DS2U] Output: \"" << output_path << "\"\n";

    try {
        arm11::BinaryLoader loader;
        if (!loader.LoadFile(input_path, exheader_path)) {
            std::cerr << "[3DS2U] Translation failed: cannot load \"" << input_path << "\"\n";
            return 1;
        }

        std::cout << "[3DS2U] Layout: " << (loader.HasExHeader() ?
            "ExeFS .code with decrypted ExHeader" : "Raw ARM; entire input is .text (no ExHeader)") << '\n'
                  << "[3DS2U] Binary length: " << loader.GetCodeBuffer().size() << " bytes\n";
        for (const auto& section : loader.GetSections()) {
            std::cout << "[3DS2U] " << section.name << ": " << section.size
                      << " initialized bytes, " << section.mapped_size << " mapped bytes, guest VA 0x"
                      << std::hex << std::uppercase << section.virtual_address
                      << ", FCRAM backing 0x" << arm11::BinaryLoader::kFCRAMBase + section.fcram_offset
                      << std::dec << '\n';
        }
        std::cout << "[3DS2U] BSS: " << loader.GetBssSize() << " zero-initialized bytes\n"
                  << "[3DS2U] ARM instruction candidates: " << loader.GetInstructionAddresses().size() << '\n'
                  << "[3DS2U] Entry point: 0x" << std::hex << std::uppercase
                  << loader.GetEntryPoint() << std::dec << '\n';
        arm11::Decoder decoder;
        const auto instructions = decoder.DecodeBuffer(loader.GetTextBuffer(), loader.GetEntryPoint());
        if (instructions.empty()) throw std::runtime_error("No ARM instructions in .text");
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
