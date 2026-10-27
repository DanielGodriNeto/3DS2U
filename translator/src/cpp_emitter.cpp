#include "cpp_emitter.hpp"

#include "alu_translator.hpp"
#include "branch_translator.hpp"
#include "cond_evaluator.hpp"
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

// Keep every statement of a translated instruction under the same condition.
void EmitInstruction(std::ostream& output, const std::string& translated,
                     const std::string& condition) {
    const bool conditional = !condition.empty();
    if (conditional) {
        output << "    if (" << condition << ") {\n";
    }
    std::istringstream lines(translated);
    for (std::string line; std::getline(lines, line);) {
        output << (conditional ? "        " : "    ") << line << '\n';
    }
    if (conditional) {
        output << "    }\n";
    }
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
                  "#include \"wiiu_engine.hpp\"\n\n"
                  "namespace arm11 {\n\n"
                  "// Block bodies inherit the current translators' supported subset.\n"
                  "// B/BX update the guest PC and return to the runtime dispatcher.\n\n";

        ALUTranslator alu;
        MemoryTranslator memory;
        BranchTranslator branch;
        for (const auto address : addresses) {
            output << "void block_" << HexAddress(address)
                   << "(arm11::CPUContext& cpu, wiiu::WiiUMemory& memory) {\n"
                      "    (void)cpu;\n"
                      "    (void)memory;\n";

            const auto& instructions = cfg.at(address).instructions;
            bool terminates = false;
            for (const auto& inst : instructions) {
                const auto cond = static_cast<std::uint8_t>(inst.opcode >> 28);
                const auto condition = ConditionEvaluator::GetConditionString(cond);
                auto translated = branch.TranslateInstruction(inst, inst.address);
                if (translated != "// Unhandled Branch") {
                    EmitInstruction(output, translated, condition);
                    if (cond == 0xEu) {
                        terminates = true;
                        break;
                    }
                    // A failed condition must reach the block's fallthrough PC.
                    continue;
                }
                translated = alu.TranslateInstruction(inst);
                if (translated == "// Unhandled Instruction") {
                    translated = memory.TranslateInstruction(inst);
                }
                EmitInstruction(output, translated, condition);
            }
            // A block split at a branch target still needs sequential fallthrough.
            if (!terminates && !instructions.empty()) {
                const std::uint32_t next_pc = instructions.back().address + 4u;
                output << "    cpu.r[15] = 0x" << HexAddress(next_pc) << "u;\n";
            }
            output << "}\n\n";
        }

        output << "} // namespace arm11\n\n"
                  "// Register translated block entry points with the Wii U engine.\n"
                  "void InitDispatcher(wiiu::WiiUEngine& engine) {\n"
                  "    (void)engine;\n";
        for (const auto address : addresses) {
            output << "    engine.RegisterBlock(0x" << HexAddress(address)
                   << "u, &arm11::block_" << HexAddress(address) << ");\n";
        }
        output << "}\n";
        output.close(); // Explicit close makes final write failures observable.
        return true;
    } catch (const std::exception& error) {
        std::cerr << "CppEmitter: " << error.what() << ": " << output_filename << '\n';
        return false;
    }
}

} // namespace arm11
