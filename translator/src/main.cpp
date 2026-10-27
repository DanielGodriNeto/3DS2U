#include "../include/arm11_types.hpp"
#include "../include/binary_loader.hpp"
#include "../include/decoder.hpp"
#include "../include/cfg_builder.hpp"
#include <iostream>
#include <fstream>

int main() {
    // 1. Create a dummy 3DS .code file
    const std::string dummy_file = "dummy_3ds_code.bin";
    std::ofstream ofs(dummy_file, std::ios::binary);
    // Block 1 (Starts at 0x08000000)
    // 0xE3A00001 = MOV R0, #1
    // 0xE0800000 = ADD R0, R0, R0
    // 0xEAFFFFFE = B . (infinite branch - ends block 1)
    
    // Block 2 (Starts at 0x0800000C)
    // 0xE3A01005 = MOV R1, #5
    // 0xE12FFF1E = BX LR (Return - ends block 2)
    uint32_t dummy_instructions[] = {
        0xE3A00001, 0xE0800000, 0xEAFFFFFE, 
        0xE3A01005, 0xE12FFF1E
    }; 
    ofs.write(reinterpret_cast<const char*>(dummy_instructions), sizeof(dummy_instructions));
    ofs.close();

    // 2. Load the binary
    arm11::BinaryLoader loader;
    if (!loader.LoadFile(dummy_file)) return 1;
    auto buffer = loader.GetCodeBuffer();

    // 3. Fetch and Decode Instructions
    arm11::Decoder decoder;
    auto instructions = decoder.DecodeBuffer(buffer, 0x08000000); 

    // 4. Build Basic Blocks
    arm11::CFGBuilder cfg;
    auto blocks = cfg.BuildBlocks(instructions);
    
    std::cout << "[3DS2U Recompiler] CFG Analysis Complete.\n";
    std::cout << "--- Basic Block Summary ---\n";
    cfg.PrintBlocks(blocks);
    std::cout << "---------------------------\n";

    return 0;
}