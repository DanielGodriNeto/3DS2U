#include "cpp_emitter.hpp"

#include "alu_translator.hpp"
#include "branch_translator.hpp"
#include "cond_evaluator.hpp"
#include "mem_translator.hpp"
#include "thumb_decoder.hpp"
#include "binary_loader.hpp"
#include "unit_publisher.hpp"
#include "vfp_translator.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace arm11 {
namespace {

namespace fs = std::filesystem;

// Unit names keep four digits, so the unit count is capped below 10000.
constexpr std::size_t kMaxUnits = 10000;

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

// The image entry must name exactly one translated block, in ARM or Thumb mode.
CodeLocation ResolveEntryBlock(const std::unordered_map<std::uint32_t, BasicBlock>& cfg,
                               std::uint32_t entry) {
    const bool arm = cfg.contains(DispatchKey({entry, InstructionSet::ARM}));
    const bool thumb = cfg.contains(DispatchKey({entry, InstructionSet::Thumb}));
    if (arm && thumb) {
        throw std::runtime_error("entry point 0x" + HexAddress(entry) +
                                 " has both ARM and Thumb translated blocks");
    }
    if (!arm && !thumb) {
        throw std::runtime_error("entry point 0x" + HexAddress(entry) +
                                 " has no translated block; check that .text decodes at the entry");
    }
    return {entry, arm ? InstructionSet::ARM : InstructionSet::Thumb};
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

// Supervisor calls: T16 0xDFxx is unconditional; ARM cond 0xF encodings are SVCs.
bool IsSvcInstruction(const Instruction& inst) {
    if (inst.state == InstructionSet::Thumb) {
        return inst.Width() == 2 && (inst.opcode & 0xFF00u) == 0xDF00u;
    }
    return (inst.opcode & 0x0F000000u) == 0x0F000000u;
}

bool BlockUsesSvc(const BasicBlock& block) {
    return std::any_of(block.instructions.begin(), block.instructions.end(), IsSvcInstruction);
}

// Returns false, after reporting, when a CFG key does not match its block start.
bool CollectSortedAddresses(const std::unordered_map<std::uint32_t, BasicBlock>& cfg,
                            std::vector<std::uint32_t>& addresses) {
    addresses.reserve(cfg.size());
    for (const auto& [address, block] : cfg) {
        if (address != DispatchKey({block.start_address, block.start.state})) {
            std::cerr << "CppEmitter: CFG key differs from block start address\n";
            return false;
        }
        addresses.push_back(address);
    }
    std::sort(addresses.begin(), addresses.end());
    return true;
}

bool AnyBlockUsesSvc(const std::unordered_map<std::uint32_t, BasicBlock>& cfg) {
    return std::any_of(cfg.begin(), cfg.end(),
                       [](const auto& entry) { return BlockUsesSvc(entry.second); });
}

std::optional<InstructionSet> FrontBlockState(
    const std::unordered_map<std::uint32_t, BasicBlock>& cfg,
    const std::vector<std::uint32_t>& addresses) {
    if (addresses.empty()) return std::nullopt;
    return cfg.at(addresses.front()).start.state;
}

void EmitFileHeader(std::ostream& output, bool uses_svc) {
    output << "#include \"arm11_types.hpp\"\n"
              "#include \"wiiu_engine.hpp\"\n\n"
              "#include <span>\n\n";
    if (uses_svc) output << "#include \"svc_handler.hpp\"\n\n";
}

void EmitImageSections(std::ostream& output, const BinaryLoader& image) {
    output << "namespace {\n";
    std::size_t index = 0;
    for (const auto& section : image.GetSections()) {
        if (section.size != 0) {
            output << "constexpr std::uint8_t image_section_" << index << "[] = {\n    ";
            const auto bytes = image.GetCodeBuffer().subspan(section.file_offset, section.size);
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

// One set of translators serves a whole emission run; they hold no per-block state.
struct BlockTranslators {
    ALUTranslator alu;
    MemoryTranslator memory;
    BranchTranslator branch;
    ThumbTranslator thumb;
    VFPTranslator vfp;
};

// Returns true when the instruction ends the block.
bool EmitSvcInstruction(std::ostream& output, const Instruction& inst) {
    const bool thumb = inst.state == InstructionSet::Thumb;
    const auto id = inst.opcode & (thumb ? 0xFFu : 0xFFFFFFu);
    const auto condition = thumb ? std::string{} :
        ConditionEvaluator::GetConditionString(static_cast<std::uint8_t>(inst.opcode >> 28));
    EmitInstruction(output, "cpu.r[15] = 0x" + HexAddress(inst.address + inst.Width()) +
        "u;\nwiiu::SvcHandler::Execute(cpu, 0x" + HexAddress(id) + "u);\nreturn;", condition);
    return condition.empty();
}

bool EmitThumbInstruction(std::ostream& output, ThumbTranslator& thumb, const Instruction& inst) {
    const auto translated = thumb.TranslateInstruction(inst);
    if (translated == "// Unhandled Thumb Instruction") {
        throw std::runtime_error("Unsupported Thumb instruction at 0x" + HexAddress(inst.address));
    }
    EmitInstruction(output, translated, "");
    return ThumbTranslator::IsTerminator(inst.opcode) &&
           !ThumbTranslator::IsConditionalBranch(inst.opcode);
}

bool EmitVfpInstruction(std::ostream& output, VFPTranslator& vfp, const Instruction& inst,
                        const std::string& condition) {
    auto translated = vfp.TranslateInstruction(inst);
    if (translated == "// Unhandled VFP Instruction") {
        // Fail closed: the word is either an invalid VFPv2 encoding (for
        // example D16-D31) or a valid form this translator lacks.  Either
        // way it must never run as a silent no-op, so the block traps if,
        // and only if, the guest instruction would execute.
        std::cerr << "CppEmitter: undefined or unsupported VFP instruction at 0x"
                  << HexAddress(inst.address) << "; emitting runtime trap\n";
        translated = "throw std::runtime_error(\"undefined or unsupported VFP instruction at 0x" +
                     HexAddress(inst.address) + "\");";
        // Condition 0xF has no ARMv6 meaning here; its "false" guard would hide the trap.
        if (inst.opcode >> 28 == 0xFu) {
            EmitInstruction(output, translated, "");
            return false;
        }
    }
    EmitInstruction(output, translated, condition);
    return false;
}

// Fail closed: an ARM word that no translator handles traps when, and only when, it would execute.
void EmitUnhandledArmInstruction(std::ostream& output, const Instruction& inst, const std::string& condition) {
    // PLD (immediate and register forms) is a cache hint with no architectural effect; compilers
    // emit it in memcpy-style loops, so trapping on it would abort working games.
    const auto word = inst.opcode;
    if ((word & 0xFF70F000u) == 0xF550F000u || (word & 0xFF70F010u) == 0xF750F000u) {
        EmitInstruction(output, "// PLD: cache hint, no architectural effect", "");
        return;
    }
    std::cerr << "CppEmitter: unsupported ARM instruction at 0x" << HexAddress(inst.address)
              << "; emitting runtime trap\n";
    const auto trap = std::string("throw std::runtime_error(\"unsupported ARM instruction at 0x") +
                      HexAddress(inst.address) + "\");";
    // Condition 0xF has no ARMv6 meaning here; its "false" guard would hide the trap.
    const bool never_condition = (inst.opcode >> 28) == 0xFu;
    EmitInstruction(output, trap, never_condition ? std::string{} : condition);
}

// Returns true when the instruction ends the block.
bool EmitArmInstruction(std::ostream& output, BlockTranslators& translators, const Instruction& inst) {
    const auto cond = static_cast<std::uint8_t>(inst.opcode >> 28);
    const bool blx_immediate = (inst.opcode & 0xFE000000u) == 0xFA000000u;
    const auto condition = blx_immediate ? "" : ConditionEvaluator::GetConditionString(cond);
    auto translated = translators.branch.TranslateInstruction(inst, inst.address);
    if (translated != "// Unhandled Branch") {
        EmitInstruction(output, translated, condition);
        return cond == 0xEu || blx_immediate;
    }
    if (VFPTranslator::IsEncoding(inst.opcode)) {
        return EmitVfpInstruction(output, translators.vfp, inst, condition);
    }
    translated = translators.alu.TranslateInstruction(inst);
    if (translated == "// Unhandled Instruction") {
        translated = translators.memory.TranslateInstruction(inst);
    }
    if (translated == "// Unhandled Instruction") {
        EmitUnhandledArmInstruction(output, inst, condition);
        return false;
    }
    EmitInstruction(output, translated, condition);
    return MemoryTranslator::IsLoadMultipleToPC(inst) && cond == 0xEu;
}

bool EmitDecodedInstruction(std::ostream& output, BlockTranslators& translators, const Instruction& inst) {
    if (IsSvcInstruction(inst)) return EmitSvcInstruction(output, inst);
    if (inst.state == InstructionSet::Thumb) return EmitThumbInstruction(output, translators.thumb, inst);
    return EmitArmInstruction(output, translators, inst);
}

void EmitBlockFunction(std::ostream& output, const BasicBlock& block, BlockTranslators& translators) {
    output << "void " << BlockName(block)
           << "(arm11::CPUContext& cpu, wiiu::WiiUMemory& memory) {\n"
              "    (void)cpu;\n"
              "    (void)memory;\n";
    bool terminates = false;
    for (const auto& inst : block.instructions) {
        if (inst.state != block.start.state) throw std::invalid_argument("Mixed modes inside a basic block");
        if (EmitDecodedInstruction(output, translators, inst)) {
            terminates = true;
            break;
        }
    }
    // A block split at a branch target still needs sequential fallthrough.
    if (!terminates && !block.instructions.empty()) {
        const std::uint32_t next_pc = block.instructions.back().address + block.instructions.back().Width();
        output << "    cpu.r[15] = 0x" << HexAddress(next_pc) << "u;\n";
    }
    output << "}\n\n";
}

void EmitImageMapping(std::ostream& output, const BinaryLoader& image) {
    output << "    auto& memory = engine.GetMemory();\n";
    std::size_t index = 0;
    for (const auto& section : image.GetSections()) {
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

// The register callback writes the per-block or per-unit registration lines.
void EmitDispatcher(std::ostream& output, const BinaryLoader* image, CodeLocation entry,
                    std::optional<InstructionSet> front_block_state,
                    const std::function<void(std::ostream&)>& register_lines) {
    output << "// Register translated block entry points with the Wii U engine.\n"
              "void InitDispatcher(wiiu::WiiUEngine& engine) {\n"
              "    (void)engine;\n";
    if (image != nullptr) EmitImageMapping(output, *image);
    register_lines(output);
    if (image != nullptr) {
        // The parsed image entry replaces any runtime-side startup address.
        output << "    engine.GetCPU().r[15] = 0x" << HexAddress(entry.address) << "u;\n"
               << "    engine.GetCPU().thumb = "
               << (entry.state == InstructionSet::Thumb ? "true" : "false") << ";\n";
    } else if (front_block_state) {
        output << "    engine.GetCPU().thumb = "
               << (*front_block_state == InstructionSet::Thumb ? "true" : "false") << ";\n";
    }
    output << "}\n";
}

void EmitBlockRegistrations(std::ostream& output,
                            const std::unordered_map<std::uint32_t, BasicBlock>& cfg,
                            const std::vector<std::uint32_t>& addresses) {
    for (const auto address : addresses) {
        output << "    engine.RegisterBlock(0x" << HexAddress(address)
               << "u, &arm11::" << BlockName(cfg.at(address)) << ");\n";
    }
}

// Unit names are %04u, so unit indices must stay below 10000.
std::string UnitIndex(std::size_t index) {
    std::ostringstream text;
    text << std::setw(4) << std::setfill('0') << index;
    return text.str();
}

std::string UnitRegisterName(std::size_t index) {
    return "RegisterTranslatedBlocks" + UnitIndex(index);
}

// A unit is a run of consecutive sorted blocks whose instruction total fits the budget.
struct Unit {
    std::vector<std::uint32_t> keys;
    std::uint64_t instructions{};
};

// Greedy packing in key order. A block is never split, so an oversized block is alone.
std::vector<Unit> PlanUnits(const std::unordered_map<std::uint32_t, BasicBlock>& cfg,
                            const std::vector<std::uint32_t>& addresses, std::uint32_t budget) {
    std::vector<Unit> units;
    for (const auto address : addresses) {
        const auto size = static_cast<std::uint64_t>(cfg.at(address).instructions.size());
        if (units.empty() || units.back().instructions + size > budget) {
            if (units.size() == kMaxUnits) {
                throw std::runtime_error("translation unit count exceeds 10000; raise the instruction budget");
            }
            units.emplace_back();
        }
        units.back().keys.push_back(address);
        units.back().instructions += size;
    }
    return units;
}

// Chunk stem: the main file name without a final ".cpp".
std::string ChunkStem(const std::string& main_output_path) {
    constexpr std::string_view kExtension = ".cpp";
    std::string stem = fs::path{main_output_path}.filename().string();
    if (stem.size() >= kExtension.size() && stem.ends_with(kExtension)) {
        stem.resize(stem.size() - kExtension.size());
    }
    return stem;
}

// Chunk path: <stem>_blocks_NNNN.cpp, in the main file's directory.
std::string ChunkFilePath(const std::string& main_output_path, std::size_t index) {
    return (fs::path{main_output_path}.parent_path() /
            (ChunkStem(main_output_path) + "_blocks_" + UnitIndex(index) + ".cpp")).string();
}

// Unit files do not need <span>, which only the dispatcher uses.
void EmitChunkHeader(std::ostream& output, bool uses_svc) {
    output << "#include \"arm11_types.hpp\"\n"
              "#include \"wiiu_engine.hpp\"\n\n";
    if (uses_svc) output << "#include \"svc_handler.hpp\"\n\n";
}

void EmitUnitFile(std::ostream& output, const Unit& unit,
                  const std::unordered_map<std::uint32_t, BasicBlock>& cfg, std::size_t index,
                  BlockTranslators& translators) {
    const bool uses_svc = std::any_of(unit.keys.begin(), unit.keys.end(),
                                      [&cfg](const auto key) { return BlockUsesSvc(cfg.at(key)); });
    EmitChunkHeader(output, uses_svc);
    output << "namespace arm11 {\nnamespace {\n\n";
    for (const auto key : unit.keys) EmitBlockFunction(output, cfg.at(key), translators);
    output << "} // namespace\n\n"
           << "void " << UnitRegisterName(index) << "(wiiu::WiiUEngine& engine) {\n";
    EmitBlockRegistrations(output, cfg, unit.keys);
    output << "}\n\n} // namespace arm11\n";
}

// The main unit declares every unit registration and calls them in index order.
void EmitMainUnit(std::ostream& output, const BinaryLoader* image, CodeLocation entry,
                  std::optional<InstructionSet> front_block_state, std::size_t unit_count) {
    EmitFileHeader(output, false);
    if (image != nullptr) EmitImageSections(output, *image);
    output << "namespace arm11 {\n";
    for (std::size_t i = 0; i < unit_count; ++i) {
        output << "void " << UnitRegisterName(i) << "(wiiu::WiiUEngine& engine);\n";
    }
    output << "} // namespace arm11\n\n";
    EmitDispatcher(output, image, entry, front_block_state, [unit_count](std::ostream& out) {
        for (std::size_t i = 0; i < unit_count; ++i) {
            out << "    arm11::" << UnitRegisterName(i) << "(engine);\n";
        }
    });
}

// Writes one staged file; exceptions cover open, write, and close failures.
void WriteTextFile(const std::string& path, const std::function<void(std::ostream&)>& write) {
    WriteStagedFile(path, write);
}

// Progress for one run. A failure is reported under the phase that raised it.
class ProgressTracker {
public:
    explicit ProgressTracker(const ProgressSink& sink) : sink_{sink} {}

    // Sets the phase that later events and failures name; emits nothing.
    void Enter(std::string_view phase) { phase_ = phase; }

    void Report(std::size_t completed, std::size_t total, std::size_t blocks_completed,
                std::size_t blocks_total) const {
        if (sink_) sink_(ProgressEvent{phase_, completed, total, blocks_completed, blocks_total, false, {}});
    }

    void Failed(const std::string& reason) const {
        if (sink_) sink_(ProgressEvent{phase_, 0, 0, 0, 0, true, reason});
    }

private:
    const ProgressSink& sink_;
    std::string_view phase_{"plan-units"};
};

// Stages every unit and the main file, one at a time, under their staging names.
void WriteStagedUnits(const UnitSetFiles& files,
                      const std::unordered_map<std::uint32_t, BasicBlock>& cfg,
                      const BinaryLoader* image, CodeLocation entry,
                      std::optional<InstructionSet> front_block_state, const std::vector<Unit>& units,
                      ProgressTracker& tracker, TempFileSet& staged) {
    BlockTranslators translators;
    std::size_t blocks_done = 0;
    tracker.Enter("emit-units");
    for (std::size_t i = 0; i < units.size(); ++i) {
        WriteTextFile(staged.Track(files.chunk_paths[i]), [&](std::ostream& out) {
            EmitUnitFile(out, units[i], cfg, i, translators);
        });
        blocks_done += units[i].keys.size();
        tracker.Report(i + 1, units.size(), blocks_done, cfg.size());
    }
    tracker.Enter("emit-dispatcher");
    WriteTextFile(staged.Track(files.main_path), [&](std::ostream& out) {
        EmitMainUnit(out, image, entry, front_block_state, units.size());
    });
    tracker.Report(units.size(), units.size(), blocks_done, cfg.size());
}

UnitSetFiles MakeUnitSetFiles(const std::string& main_output_path, const std::vector<Unit>& units) {
    UnitSetFiles files;
    files.main_path = main_output_path;
    files.chunk_stem = ChunkStem(main_output_path);
    for (std::size_t i = 0; i < units.size(); ++i) {
        files.chunk_paths.push_back(ChunkFilePath(main_output_path, i));
        files.expected_keys.push_back(units[i].keys);
    }
    return files;
}

// Stage, verify, then publish. Nothing final is touched until every staged file has been verified.
void PublishUnitSet(const std::string& main_output_path,
                    const std::unordered_map<std::uint32_t, BasicBlock>& cfg,
                    const BinaryLoader* image, CodeLocation entry,
                    std::optional<InstructionSet> front_block_state, const std::vector<Unit>& units,
                    ProgressTracker& tracker) {
    const auto files = MakeUnitSetFiles(main_output_path, units);
    // Refuse a bad final or stale match before any staged file is created.
    tracker.Enter("publish");
    CheckPublishable(files);
    TempFileSet staged;
    WriteStagedUnits(files, cfg, image, entry, front_block_state, units, tracker, staged);
    tracker.Enter("verify-registration");
    VerifyStagedFiles(files);
    tracker.Report(units.size(), units.size(), cfg.size(), cfg.size());
    tracker.Enter("publish");
    PublishStagedFiles(files);
    tracker.Report(units.size(), units.size(), cfg.size(), cfg.size());
}

void ValidateUnitOptions(UnitOptions options) {
    if (options.max_instructions_per_unit == 0) {
        throw std::invalid_argument("unit instruction budget must be at least 1");
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
        if (!CollectSortedAddresses(cfg, addresses)) return false;
        const bool uses_svc = AnyBlockUsesSvc(cfg);
        // Resolve before opening the output so a rejected entry leaves no file behind.
        const auto entry = image != nullptr ? ResolveEntryBlock(cfg, image->GetEntryPoint()) : CodeLocation{};

        // Stream exceptions cover open, write, flush, and close failures.
        std::ofstream output;
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.open(output_filename, std::ios::out | std::ios::trunc);

        EmitFileHeader(output, uses_svc);
        if (image != nullptr) EmitImageSections(output, *image);
        output << "namespace arm11 {\n\n"
                  "// Block bodies inherit the current translators' supported subset.\n"
                  "// B/BL/BX and LDM-to-PC return control to the runtime dispatcher.\n\n";

        BlockTranslators translators;
        for (const auto address : addresses) {
            EmitBlockFunction(output, cfg.at(address), translators);
        }
        output << "} // namespace arm11\n\n";
        EmitDispatcher(output, image, entry, FrontBlockState(cfg, addresses),
                       [&](std::ostream& out) { EmitBlockRegistrations(out, cfg, addresses); });
        output.close(); // Explicit close makes final write failures observable.
        return true;
    } catch (const std::exception& error) {
        std::cerr << "CppEmitter: " << error.what() << ": " << output_filename << '\n';
        return false;
    }
}

bool CppEmitter::EmitTranslatedUnits(
    const std::string& main_output_path,
    const std::unordered_map<std::uint32_t, BasicBlock>& cfg,
    const BinaryLoader* image,
    UnitOptions options,
    const ProgressSink& progress) const {
    ProgressTracker tracker{progress};
    try {
        ValidateUnitOptions(options);
        std::vector<std::uint32_t> addresses;
        if (!CollectSortedAddresses(cfg, addresses)) {
            tracker.Failed("CFG key differs from block start address");
            return false;
        }
        // Entry and planning finish before the first file is opened.
        const auto entry = image != nullptr ? ResolveEntryBlock(cfg, image->GetEntryPoint()) : CodeLocation{};
        const auto units = PlanUnits(cfg, addresses, options.max_instructions_per_unit);
        tracker.Enter("plan-units");
        tracker.Report(units.size(), units.size(), cfg.size(), cfg.size());
        PublishUnitSet(main_output_path, cfg, image, entry, FrontBlockState(cfg, addresses), units, tracker);
        tracker.Enter("complete");
        tracker.Report(units.size(), units.size(), cfg.size(), cfg.size());
        return true;
    } catch (const std::exception& error) {
        tracker.Failed(error.what());
        std::cerr << "CppEmitter: " << error.what() << ": " << main_output_path << '\n';
        return false;
    }
}

} // namespace arm11
