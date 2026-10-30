#include "cpp_emitter.hpp"

#include "alu_translator.hpp"
#include "branch_translator.hpp"
#include "cond_evaluator.hpp"
#include "mem_translator.hpp"
#include "thumb_decoder.hpp"
#include "binary_loader.hpp"
#include "vfp_translator.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
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

std::string BlockName(const BasicBlock& block) {
    return std::string{block.start.state == InstructionSet::Thumb ? "thumb_block_" : "block_"} +
           HexAddress(block.start_address);
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
    const std::unordered_map<std::uint32_t, BasicBlock>& cfg,
    const BinaryLoader* image) const {
    try {
        // Sort the unordered CFG so identical inputs yield identical source files.
        std::vector<std::uint32_t> addresses;
        bool uses_svc = false;
        addresses.reserve(cfg.size());
        for (const auto& [address, block] : cfg) {
            if (address != DispatchKey({block.start_address, block.start.state})) {
                std::cerr << "CppEmitter: CFG key differs from block start address\n";
                return false;
            }
            addresses.push_back(address);
            for (const auto& inst : block.instructions)
                uses_svc |= inst.state == InstructionSet::Thumb ? inst.Width() == 2 && (inst.opcode & 0xFF00u) == 0xDF00u :
                            (inst.opcode & 0x0F000000u) == 0x0F000000u;
        }
        std::sort(addresses.begin(), addresses.end());

        // Stream exceptions cover open, write, flush, and close failures.
        std::ofstream output;
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.open(output_filename, std::ios::out | std::ios::trunc);

        output << "#include \"arm11_types.hpp\"\n"
                  "#include \"wiiu_engine.hpp\"\n\n"
                  "#include <span>\n\n";
        if (uses_svc) output << "#include \"svc_handler.hpp\"\n\n";
        if (image != nullptr) {
            output << "namespace {\n";
            std::size_t index = 0;
            for (const auto& section : image->GetSections()) {
                if (section.size != 0) {
                    output << "constexpr std::uint8_t image_section_" << index << "[] = {\n    ";
                    const auto bytes = image->GetCodeBuffer().subspan(section.file_offset, section.size);
                    for (std::size_t i = 0; i < bytes.size(); ++i) {
                        output << static_cast<unsigned>(bytes[i]) << ',';
                        if ((i + 1) % 16 == 0) output << "\n    ";
                    }
                    output << "\n};\n";
                }
                ++index;
            }
            output << "} // namespace\n\n";
        }
        output << "namespace arm11 {\n\n"
                  "// Block bodies inherit the current translators' supported subset.\n"
                  "// B/BL/BX and LDM-to-PC return control to the runtime dispatcher.\n\n";

        ALUTranslator alu;
        MemoryTranslator memory;
        BranchTranslator branch;
        ThumbTranslator thumb;
        VFPTranslator vfp;
        for (const auto address : addresses) {
            const auto& block = cfg.at(address);
            output << "void " << BlockName(block)
                   << "(arm11::CPUContext& cpu, wiiu::WiiUMemory& memory) {\n"
                      "    (void)cpu;\n"
                      "    (void)memory;\n";

            const auto& instructions = cfg.at(address).instructions;
            bool terminates = false;
            for (const auto& inst : instructions) {
                if (inst.state != block.start.state) throw std::invalid_argument("Mixed modes inside a basic block");
                const bool t16_svc = inst.state == InstructionSet::Thumb && inst.Width() == 2 && (inst.opcode & 0xFF00u) == 0xDF00u;
                const bool arm_svc = inst.state == InstructionSet::ARM && (inst.opcode & 0x0F000000u) == 0x0F000000u;
                if (t16_svc || arm_svc) {
                    const auto id = inst.opcode & (t16_svc ? 0xFFu : 0xFFFFFFu);
                    const auto condition = t16_svc ? std::string{} :
                        ConditionEvaluator::GetConditionString(static_cast<std::uint8_t>(inst.opcode >> 28));
                    EmitInstruction(output, "cpu.r[15] = 0x" + HexAddress(inst.address + inst.Width()) +
                        "u;\nwiiu::SvcHandler::Execute(cpu, 0x" + HexAddress(id) + "u);\nreturn;", condition);
                    if (condition.empty()) { terminates = true; break; }
                    continue;
                }
                if (inst.state == InstructionSet::Thumb) {
                    const auto translated = thumb.TranslateInstruction(inst);
                    if (translated == "// Unhandled Thumb Instruction") {
                        throw std::runtime_error("Unsupported Thumb instruction at 0x" + HexAddress(inst.address));
                    }
                    EmitInstruction(output, translated, "");
                    if (ThumbTranslator::IsTerminator(inst.opcode) &&
                        !ThumbTranslator::IsConditionalBranch(inst.opcode)) {
                        terminates = true;
                        break;
                    }
                    continue;
                }
                const auto cond = static_cast<std::uint8_t>(inst.opcode >> 28);
                const bool blx_immediate = (inst.opcode & 0xFE000000u) == 0xFA000000u;
                const auto condition = blx_immediate ? "" : ConditionEvaluator::GetConditionString(cond);
                auto translated = branch.TranslateInstruction(inst, inst.address);
                if (translated != "// Unhandled Branch") {
                    EmitInstruction(output, translated, condition);
                    if (cond == 0xEu || blx_immediate) {
                        terminates = true;
                        break;
                    }
                    // A failed condition must reach the block's fallthrough PC.
                    continue;
                }
                if (VFPTranslator::IsEncoding(inst.opcode)) {
                    translated = vfp.TranslateInstruction(inst);
                    if (translated == "// Unhandled VFP Instruction") {
                        // Keep translation deterministic when an image contains an
                        // invalid VFPv2 register encoding.  ARM11 cannot address
                        // D16-D31, so this is an undefined guest instruction rather
                        // than a host-emitter failure.
                        std::cerr << "CppEmitter: unsupported VFPv2 instruction at 0x"
                                  << HexAddress(inst.address) << "; emitting fallback\n";
                    }
                    EmitInstruction(output, translated, condition);
                    continue;
                }
                translated = alu.TranslateInstruction(inst);
                if (translated == "// Unhandled Instruction") {
                    translated = memory.TranslateInstruction(inst);
                }
                EmitInstruction(output, translated, condition);
                if (MemoryTranslator::IsLoadMultipleToPC(inst) && cond == 0xEu) {
                    terminates = true;
                    break;
                }
            }
            // A block split at a branch target still needs sequential fallthrough.
            if (!terminates && !instructions.empty()) {
                const std::uint32_t next_pc = instructions.back().address + instructions.back().Width();
                output << "    cpu.r[15] = 0x" << HexAddress(next_pc) << "u;\n";
            }
            output << "}\n\n";
        }

        output << "} // namespace arm11\n\n"
                  "// Register translated block entry points with the Wii U engine.\n"
                  "void InitDispatcher(wiiu::WiiUEngine& engine) {\n"
                  "    (void)engine;\n";
        if (image != nullptr) {
            output << "    auto& memory = engine.GetMemory();\n";
            std::size_t index = 0;
            for (const auto& section : image->GetSections()) {
                if (section.mapped_size != 0) {
                    output << "    memory.MapImageRange(" << section.virtual_address << "u, "
                           << section.fcram_offset << "u, " << section.mapped_size << "u);\n";
                    output << "    memory.ReserveImageRange(" << section.virtual_address << "u, "
                           << section.mapped_size << "u);\n";
                    output << "    memory.ClearRange(" << section.virtual_address << "u, "
                           << section.mapped_size << "u);\n";
                }
                if (section.size != 0) {
                    output << "    memory.LoadImage(" << section.virtual_address
                           << "u, std::span{image_section_" << index << "});\n";
                }
                ++index;
            }
        }
        for (const auto address : addresses) {
            output << "    engine.RegisterBlock(0x" << HexAddress(address)
                   << "u, &arm11::" << BlockName(cfg.at(address)) << ");\n";
        }
        if (!addresses.empty()) {
            output << "    engine.GetCPU().thumb = "
                   << (cfg.at(addresses.front()).start.state == InstructionSet::Thumb ? "true" : "false")
                   << ";\n";
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
