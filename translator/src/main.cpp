#include "../include/arm11_types.hpp"
#include "../include/binary_loader.hpp"
#include "../include/decoder.hpp"
#include "../include/cfg_builder.hpp"
#include "../include/cpp_emitter.hpp"
#include <iostream>
#include <fstream>

int main() {
    // 1. Criar binario contendo instrucoes ARM11 variadas
    const std::string dummy_file = "dummy_program.bin";
    std::ofstream ofs(dummy_file, std::ios::binary);
    
    uint32_t dummy_instructions[] = {
        0xE1A01002, // MOV R1, R2
        0xE0813002, // ADD R3, R1, R2
        0xE5901010, // LDR R1, [R0, #0x10]
        0xE5801020, // STR R1, [R0, #0x20]
        0xEAFFFFFE  // B . (Branch - Finaliza bloco)
    }; 
    ofs.write(reinterpret_cast<const char*>(dummy_instructions), sizeof(dummy_instructions));
    ofs.close();

    // 2. Carregar o Binario
    arm11::BinaryLoader loader;
    if (!loader.LoadFile(dummy_file)) {
        std::cerr << "[3DS2U] Falha ao carregar o arquivo binario.\n";
        return 1;
    }

    // 3. Decodificar Instrucoes
    arm11::Decoder decoder;
    auto instructions = decoder.DecodeBuffer(loader.GetCodeBuffer(), 0x08000000);

    // 4. Construir o Grafo de Fluxo de Controle (CFG)
    arm11::CFGBuilder cfg;
    auto blocks = cfg.BuildBlocks(instructions);

    // 5. Emitir o Codigo C++ Traduzido para Arquivo
    arm11::CppEmitter emitter;
    const std::string output_cpp = "translated_game.cpp";
    if (emitter.EmitTranslatedFile(output_cpp, blocks)) {
        std::cout << "[3DS2U Recompiler] SUCESSO! Pipeline do Modulo 1 concluido.\n";
        std::cout << "Arquivo C++ gerado com sucesso: " << output_cpp << "\n";
    } else {
        std::cerr << "[3DS2U Recompiler] Falha ao emitir arquivo C++.\n";
        return 1;
    }

    return 0;
}