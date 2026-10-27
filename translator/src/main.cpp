#include "../include/arm11_types.hpp"
#include "../include/binary_loader.hpp"
#include "../include/decoder.hpp"
#include <iostream>
#include <fstream>

int main() {
    // 1. Create a dummy 3DS .code file
    const std::string dummy_file = "dummy_3ds_code.bin";
    std::ofstream ofs(dummy_file, std::ios::binary);
    // 0xE3A00001 = MOV R0, #1
    // 0xE0800000 = ADD R0, R0, R0
    // 0xEAFFFFFE = B . (infinite branch/loop)
    uint32_t dummy_instructions[] = {0xE3A00001, 0xE0800000, 0xEAFFFFFE}; 
    ofs.write(reinterpret_cast<const char*>(dummy_instructions), sizeof(dummy_instructions));
    ofs.close();

    // 2. Load the binary
    arm11::BinaryLoader loader;
    if (!loader.LoadFile(dummy_file)) {
        std::cerr << "[3DS2U Recompiler] Failed to load binary.\n";
        return 1;
    }

    auto buffer = loader.GetCodeBuffer();
    std::cout << "[3DS2U Recompiler] Loaded " << buffer.size() << " bytes.\n\n";

    // 3. Fetch and Decode Instructions
    arm11::Decoder decoder;
    // 3DS binaries usually load at 0x08000000 or 0x00100000 depending on the executable type
    auto instructions = decoder.DecodeBuffer(buffer, 0x08000000); 

    std::cout << "--- Decoded ARM11 Instruction Stream ---\n";
    for (const auto& inst : instructions) {
        decoder.PrintInstruction(inst);
    }
    std::cout << "----------------------------------------\n";

    return 0;
}