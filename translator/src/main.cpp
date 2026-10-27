#include "../include/arm11_types.hpp"
#include "../include/binary_loader.hpp"
#include "../include/decoder.hpp"
#include "../include/cfg_builder.hpp"
#include "../include/alu_translator.hpp"
#include <iostream>
#include <fstream>

int main() {
    // 1. Criar binário com instruções ALU Register-to-Register
    const std::string dummy_file = "dummy_alu.bin";
    std::ofstream ofs(dummy_file, std::ios::binary);
    
    uint32_t dummy_instructions[] = {
        0xE1A01002, // MOV R1, R2
        0xE0813002, // ADD R3, R1, R2
        0xE0434001, // SUB R4, R3, R1
        0xEAFFFFFE  // B . (Salto infinito - Encerra o bloco)
    }; 
    ofs.write(reinterpret_cast<const char*>(dummy_instructions), sizeof(dummy_instructions));
    ofs.close();

    // 2. Carregar e Decodificar
    arm11::BinaryLoader loader;
    if (!loader.LoadFile(dummy_file)) return 1;
    
    arm11::Decoder decoder;
    auto instructions = decoder.DecodeBuffer(loader.GetCodeBuffer(), 0x08000000); 

    // 3. Traduzir instruções para C++
    arm11::ALUTranslator alu;
    std::cout << "[3DS2U Recompiler] Gerando Codigo C++ (ALU)...\n";
    std::cout << "----------------------------------------\n";
    for (const auto& inst : instructions) {
        decoder.PrintInstruction(inst);
        std::string cpp_code = alu.TranslateInstruction(inst);
        std::cout << "   -> " << cpp_code << "\n";
    }
    std::cout << "----------------------------------------\n";

    return 0;
}