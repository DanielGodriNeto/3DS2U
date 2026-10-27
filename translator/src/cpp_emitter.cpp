#include "cpp_emitter.hpp"

#include "alu_translator.hpp"
#include "mem_translator.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

namespace arm11 {
namespace {

// Fixed-width names preserve the guest PC and remain valid C++ identifiers.
std::string HexAddress(std::uint32_t address) {
    std::ostringstream text;
    text << std::hex << std::uppercase << std::setfill('0') << std::setw(8)
         << address;
    return text.str();
}

} // namespace

bool CppEmitter::EmitTranslatedFile(
    const std::string& output_filename,
    const std::unordered_map<std::uint32_t, BasicBlock>& cfg) const {
    try {
        // Sort the unordered CFG so identical inputs yield identical source files.
        std::vector<std::uint32_t> addresses;
        addresses.reserve(cfg.size());
        for (const auto& [address, block] : cfg) {
            if (address != block.start_address) {
                std::cerr << "CppEmitter: CFG key differs from block start address\n";
                return false;
            }
            addresses.push_back(address);
        }
        std::sort(addresses.begin(), addresses.end());

        // Stream exceptions cover open, write, flush, and close failures.
        std::ofstream output;
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.open(output_filename, std::ios::out | std::ios::trunc);

        output << "#include \"arm11_types.hpp\"\n"
                  "#include <cstdint>\n"
                  "#include <unordered_map>\n\n"
                  "namespace arm11 {\n\n"
                  "using BlockFunction = void (*)(arm11::CPUContext&, arm11::Memory&);\n"
                  "using BlockDispatcher = std::unordered_map<std::uint32_t, BlockFunction>;\n\n"
                  "// Block bodies inherit the current translators' supported subset.\n"
                  "// Branch execution and dispatcher scheduling are implemented separately.\n\n";

        ALUTranslator alu;
        MemoryTranslator memory;
        for (const auto address : addresses) {
            output << "void block_" << HexAddress(address)
                   << "(arm11::CPUContext& cpu, arm11::Memory& memory) {\n"
                      "    (void)cpu;\n"
                      "    (void)memory;\n";

            for (const auto& inst : cfg.at(address).instructions) {
                auto translated = alu.TranslateInstruction(inst);
                if (translated == "// Unhandled Instruction") {
                    translated = memory.TranslateInstruction(inst);
                }
                output << "    " << translated << '\n';
            }
            output << "}\n\n";
        }

        output << "// Replace the dispatch table with the emitted block entry points.\n"
                  "void InitDispatcher(BlockDispatcher& dispatcher) {\n"
                  "    dispatcher.clear();\n"
                  "    dispatcher.reserve(" << addresses.size() << ");\n";
        for (const auto address : addresses) {
            output << "    dispatcher.emplace(0x" << HexAddress(address)
                   << "u, &block_" << HexAddress(address) << ");\n";
        }
        output << "}\n\n} // namespace arm11\n";
        output.close(); // Explicit close makes final write failures observable.
        return true;
    } catch (const std::exception& error) {
        std::cerr << "CppEmitter: " << error.what() << ": " << output_filename << '\n';
        return false;
    }
}

} // namespace arm11
