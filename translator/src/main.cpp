#include "../include/arm11_types.hpp"
#include <iostream>

int main() {
    arm11::CPUContext cpu{};
    cpu.r[0] = 0xDEADBEEF;

    std::cout << "[3DS2U Recompiler] ARM11 Types initialized successfully.\n";
    std::cout << "R0 Register Value: 0x" << std::hex << cpu.r[0] << "\n";

    return 0;
}