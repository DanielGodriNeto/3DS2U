#include "../include/arm11_types.hpp"
#include "../include/binary_loader.hpp"
#include <iostream>
#include <fstream>

int main() {
    // 1. Create a dummy 3DS .code file for testing
    const std::string dummy_file = "dummy_3ds_code.bin";
    std::ofstream ofs(dummy_file, std::ios::binary);
    uint32_t dummy_instructions[] = {0xE3A00001, 0xE0800000}; // MOV R0, #1; ADD R0, R0, R0
    ofs.write(reinterpret_cast<const char*>(dummy_instructions), sizeof(dummy_instructions));
    ofs.close();

    // 2. Test the Binary Loader
    arm11::BinaryLoader loader;
    if (loader.LoadFile(dummy_file)) {
        auto buffer = loader.GetCodeBuffer();
        std::cout << "[3DS2U Recompiler] Successfully loaded dummy .code file.\n";
        std::cout << "Buffer size: " << buffer.size() << " bytes.\n";
    } else {
        std::cerr << "[3DS2U Recompiler] Failed to load binary.\n";
        return 1;
    }

    return 0;
}