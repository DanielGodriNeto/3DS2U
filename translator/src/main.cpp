#include "../include/arm11_types.hpp"
#include "../include/binary_loader.hpp"
#include "../include/decoder.hpp"
#include "../include/cfg_builder.hpp"
#include "../include/alu_translator.hpp"
#include "../include/mem_translator.hpp"
#include <iostream>
#include <fstream>

int main() {
    // 1. Criar binário com instruções LDR e STR
    const std::string dummy_file = "dummy_mem.bin";
    std::ofstream ofs(dummy_file, std::ios::binary);
    
    uint32_t dummy_instructions[] = {
        0xE5901010, // LDR R1, [R0, #0x10] -> Carrega de (R0 + 16 bytes) para R1
        0xE5801020, // STR R1, [R0, #0x20] -> Salva R1 em (R0 + 32 bytes)
        0xEAFFFFFE  // B . (Salto infinito - Encerra o bloco)
    }; 
    ofs.write(reinterpret_cast<const char*>(dummy_instructions), sizeof(dummy_instructions));
    ofs.close();

    // 2. Carregar e Decodificar
    arm11::BinaryLoader loader;
    if (!loader.LoadFile(dummy_file)) return 1;
    
    arm11::Decoder decoder;
    auto instructions = decoder.DecodeBuffer(loader.GetCodeBuffer(), 0x08000000); 

    // 3. Traduzir instruções de Memória (LDR/STR)
    arm11::MemoryTranslator mem_trans;
    std::cout << "[3DS2U Recompiler] Gerando Codigo C++ (Memory LDR/STR)...\n";
    std::cout << "----------------------------------------\n";
    for (const auto& inst : instructions) {
        decoder.PrintInstruction(inst);
        std::string cpp_code = mem_trans.TranslateInstruction(inst);
        std::cout << "   -> " << cpp_code << "\n";
    }
    std::cout << "----------------------------------------\n";

    return 0;
}