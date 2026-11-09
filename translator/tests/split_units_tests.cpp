// Host tests for bounded translation units (EmitTranslatedUnits).
// Synthetic ARM images come from a fixed-seed LCG; the CFG is built with the real
// Decoder and CFGBuilder. argv[1] is a scratch work directory.
#include "binary_loader.hpp"
#include "cfg_builder.hpp"
#include "cpp_emitter.hpp"
#include "decoder.hpp"
#include "progress_sink.hpp"
#include "unit_publisher.hpp"
#include "thumb_decoder.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
using Cfg = std::unordered_map<std::uint32_t, arm11::BasicBlock>;
using Registration = std::pair<std::uint32_t, std::string>;

constexpr std::uint32_t kBase = 0x00100000u;
constexpr std::uint32_t kBudget = 64u;
constexpr std::size_t kBoundedWords = 1200u;
constexpr std::size_t kAluPrefix = 70u;
constexpr std::size_t kSvcIndex = 900u;
const std::string kRegisterMarker = "engine.RegisterBlock(0x";

void Check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}

void Append32(Bytes& bytes, std::uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(word >> (i * 8)));
}

void Append16(Bytes& bytes, std::uint32_t half) {
    bytes.push_back(static_cast<std::uint8_t>(half & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>((half >> 8) & 0xFFu));
}

void Put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (i * 8));
}

void SaveBytes(const fs::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    Check(file.is_open(), "cannot write " + path.string());
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string ReadText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    Check(file.is_open(), "cannot read " + path.string());
    std::ostringstream text;
    text << file.rdbuf();
    // Windows text-mode writers emit CRLF; compare on LF so line checks hold on every host.
    auto content = text.str();
    content.erase(std::remove(content.begin(), content.end(), '\r'), content.end());
    return content;
}

void Reset(const fs::path& dir) {
    fs::remove_all(dir);
    fs::create_directories(dir);
}

std::size_t EntryCount(const fs::path& dir) {
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(dir)) ++count;
    return count;
}

// Decrypted ExHeader with one text section at kBase, as in entry_dispatch_tests.cpp.
Bytes MakeExHeader(std::uint32_t text_size) {
    Bytes header(0x400, 0);
    Put32(header, 0x10, kBase);
    Put32(header, 0x14, (text_size + 0xFFFu) / 0x1000u);
    Put32(header, 0x18, text_size);
    return header;
}

// Writes the image into input_dir and loads it through BinaryLoader.
void LoadImage(const fs::path& input_dir, const Bytes& code, std::uint32_t alignment,
               arm11::BinaryLoader& image) {
    Reset(input_dir);
    const auto code_path = (input_dir / "image.code").string();
    const auto header_path = (input_dir / "image.exheader").string();
    SaveBytes(code_path, code);
    SaveBytes(header_path, MakeExHeader(static_cast<std::uint32_t>(code.size())));
    Check(image.LoadFile(code_path, header_path, alignment), "BinaryLoader::LoadFile failed");
}

// Deterministic 32-bit LCG (Numerical Recipes constants).
class Lcg {
public:
    explicit Lcg(std::uint32_t seed) : state_{seed} {}
    std::uint32_t Next() {
        state_ = state_ * 1664525u + 1013904223u;
        return state_;
    }

private:
    std::uint32_t state_;
};

// Data-processing MOV/ADD/SUB/EOR/ORR, S clear, immediate-shift register form (bits 4 and 7 clear).
std::uint32_t AluWord(Lcg& lcg) {
    static constexpr std::uint32_t kOps[] = {0x4u, 0x2u, 0xCu, 0xDu};
    const std::uint32_t cond = (lcg.Next() >> 8) % 15u;
    const std::uint32_t op = kOps[(lcg.Next() >> 16) % 4u];
    const std::uint32_t r = lcg.Next();
    const std::uint32_t rn = (r >> 4) & 0xFu;
    const std::uint32_t rd = (r >> 8) & 0xFu;
    const std::uint32_t op2 = (r >> 12) & 0xF6Fu;
    return (cond << 28) | (op << 21) | (rn << 16) | (rd << 12) | op2;
}

// B or Bcc with imm24 = k, so the target is pc + 8 + 4k with k >= 1.
std::uint32_t BranchWord(std::uint32_t cond, std::uint32_t k) {
    return (cond << 28) | 0x0A000000u | k;
}

// Forward branch within the image (1 in 5 words), otherwise ALU. No memory instructions.
std::uint32_t SyntheticWord(Lcg& lcg, std::size_t index, std::size_t count) {
    const std::size_t room = count > index + 3 ? count - index - 3 : 0;
    if (room == 0 || lcg.Next() % 5u != 0u) return AluWord(lcg);
    const std::uint32_t cond = (lcg.Next() >> 8) % 15u;
    const auto span = static_cast<std::uint32_t>(std::min<std::size_t>(room, 4));
    return BranchWord(cond, 1u + lcg.Next() % span);
}

// The first alu_prefix words are ALU, then word alu_prefix is an unconditional forward B.
std::vector<std::uint32_t> SyntheticWords(std::size_t count, std::uint32_t seed, std::size_t alu_prefix) {
    Lcg lcg{seed};
    std::vector<std::uint32_t> words;
    words.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        words.push_back(i < alu_prefix ? AluWord(lcg) : SyntheticWord(lcg, i, count));
    }
    if (alu_prefix < count) words[alu_prefix] = BranchWord(0xEu, 1u);
    return words;
}

// Bounded fixture: a 71-instruction block at the start, then mixed blocks.
std::vector<std::uint32_t> BoundedWords() {
    return SyntheticWords(kBoundedWords, 0x5EED0001u, kAluPrefix);
}

Bytes WordsToBytes(const std::vector<std::uint32_t>& words) {
    Bytes bytes;
    bytes.reserve(words.size() * 4);
    for (const auto word : words) Append32(bytes, word);
    return bytes;
}

// Two halfwords per Thumb instruction (MOVS, ADDS, BEQ, MOVS, B, MOVS).
Bytes ThumbCode() {
    Bytes code;
    for (const std::uint32_t half : {0x2000u, 0x3001u, 0xD000u, 0x2102u, 0xE000u, 0x2203u}) {
        Append16(code, half);
    }
    return code;
}

Cfg ArmCfg(const Bytes& code, std::uint32_t base) {
    arm11::Decoder decoder;
    arm11::CFGBuilder builder;
    return builder.BuildBlocks(decoder.DecodeBuffer(code, base));
}

Cfg ThumbCfgFromCode(const Bytes& code, std::uint32_t base) {
    arm11::ThumbTranslator thumb;
    arm11::CFGBuilder builder;
    return builder.BuildBlocks(thumb.DecodeBuffer(code, base));
}

// The same blocks, inserted in descending key order; reserve() changes the bucket layout.
Cfg ReversedCfg(const Cfg& source, std::size_t reserve_hint) {
    std::vector<std::uint32_t> keys;
    keys.reserve(source.size());
    for (const auto& [key, block] : source) keys.push_back(key);
    std::sort(keys.begin(), keys.end(), std::greater<>{});
    Cfg reversed;
    reversed.reserve(reserve_hint);
    for (const auto key : keys) reversed.emplace(key, source.at(key));
    return reversed;
}

// Single-instruction blocks at consecutive addresses, built without the decoder.
Cfg SingleInstructionBlocks(std::size_t count) {
    Cfg cfg;
    cfg.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto address = static_cast<std::uint32_t>(kBase + 4u * i);
        arm11::BasicBlock block;
        block.start_address = address;
        block.start = {address, arm11::InstructionSet::ARM};
        block.instructions.push_back(arm11::Instruction{0xE1A00000u, address, arm11::InstructionSet::ARM});
        cfg.emplace(address, std::move(block));
    }
    return cfg;
}

std::string Index4(std::size_t index) {
    std::ostringstream text;
    text << std::setw(4) << std::setfill('0') << index;
    return text.str();
}

fs::path MainPath(const fs::path& dir) { return dir / "translated_game.cpp"; }

fs::path ChunkPath(const fs::path& dir, std::size_t index) {
    return dir / ("translated_game_blocks_" + Index4(index) + ".cpp");
}

std::size_t ChunkEntryCount(const fs::path& dir) {
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().filename().string().rfind("translated_game_blocks_", 0) == 0) ++count;
    }
    return count;
}

// Every file in dir, keyed by name. Subdirectories are not expected.
std::map<std::string, std::string> Snapshot(const fs::path& dir) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        Check(entry.is_regular_file(), "unexpected non-file entry " + entry.path().string());
        files.emplace(entry.path().filename().string(), ReadText(entry.path()));
    }
    return files;
}

bool EmitUnits(const fs::path& main_path, const Cfg& cfg, const arm11::BinaryLoader* image,
               std::uint32_t budget, const arm11::ProgressSink& progress = arm11::ProgressSink{}) {
    fs::create_directories(main_path.parent_path());
    arm11::CppEmitter emitter;
    return emitter.EmitTranslatedUnits(main_path.string(), cfg, image,
                                       arm11::UnitOptions{budget}, progress);
}

// Parses "engine.RegisterBlock(0xKEYu, &arm11::NAME);" lines in file order.
std::vector<Registration> ParseRegistrations(const std::string& text) {
    std::vector<Registration> registrations;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        const auto pos = line.find(kRegisterMarker);
        if (pos == std::string::npos) continue;
        const auto key = static_cast<std::uint32_t>(
            std::stoul(line.substr(pos + kRegisterMarker.size(), 8), nullptr, 16));
        const auto name_begin = line.find("&arm11::", pos) + std::string{"&arm11::"}.size();
        const auto name_end = line.find(')', name_begin);
        registrations.emplace_back(key, line.substr(name_begin, name_end - name_begin));
    }
    return registrations;
}

// Keys registered by each chunk, in unit index order. Requires chunk 0000 to exist.
std::vector<std::vector<std::uint32_t>> UnitKeys(const fs::path& dir) {
    Check(fs::exists(ChunkPath(dir, 0)), "chunk 0000 exists");
    const auto count = ChunkEntryCount(dir);
    std::vector<std::vector<std::uint32_t>> units;
    for (std::size_t i = 0; i < count; ++i) {
        std::vector<std::uint32_t> keys;
        for (const auto& [key, name] : ParseRegistrations(ReadText(ChunkPath(dir, i)))) keys.push_back(key);
        units.push_back(std::move(keys));
    }
    return units;
}

std::size_t InstructionCount(const Cfg& cfg, std::uint32_t key) {
    return cfg.at(key).instructions.size();
}

std::size_t UnitInstructions(const Cfg& cfg, const std::vector<std::uint32_t>& keys) {
    std::size_t sum = 0;
    for (const auto key : keys) sum += InstructionCount(cfg, key);
    return sum;
}

// Text of the InitDispatcher function, without the per-block registration and unit call lines.
std::string DispatcherTail(const std::string& text) {
    const auto begin = text.find("void InitDispatcher(");
    Check(begin != std::string::npos, "InitDispatcher missing");
    std::istringstream lines(text.substr(begin));
    std::string kept;
    for (std::string line; std::getline(lines, line);) {
        if (line.find("RegisterBlock(") != std::string::npos) continue;
        if (line.find("RegisterTranslatedBlocks") != std::string::npos) continue;
        kept += line + '\n';
    }
    return kept;
}

// Maps each "void block_...(arm11::CPUContext&" name to its text through the closing "}" line.
std::map<std::string, std::string> ExtractBodies(const std::string& text) {
    std::map<std::string, std::string> bodies;
    std::istringstream lines(text);
    std::string body;
    std::string name;
    for (std::string line; std::getline(lines, line);) {
        if (name.empty() && line.rfind("void ", 0) == 0 &&
            line.find("(arm11::CPUContext& cpu") != std::string::npos) {
            name = line.substr(5, line.find('(') - 5);
            body.clear();
        }
        if (name.empty()) continue;
        body += line + '\n';
        if (line == "}") {
            Check(bodies.emplace(name, body).second, "duplicate block body " + name);
            name.clear();
        }
    }
    return bodies;
}

// Registration order of the split main file: the RegisterTranslatedBlocksNNNN calls, expanded.
std::vector<Registration> ExpandedSplitRegistrations(const fs::path& split_dir) {
    std::vector<Registration> expanded;
    const std::string call_prefix = "    arm11::RegisterTranslatedBlocks";
    std::istringstream lines(ReadText(MainPath(split_dir)));
    std::size_t expected_index = 0;
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind(call_prefix, 0) != 0) continue;
        const auto index = static_cast<std::size_t>(std::stoul(line.substr(call_prefix.size(), 4)));
        Check(index == expected_index, "unit call " + std::to_string(index) + " out of order");
        const auto chunk = ParseRegistrations(ReadText(ChunkPath(split_dir, index)));
        expanded.insert(expanded.end(), chunk.begin(), chunk.end());
        ++expected_index;
    }
    Check(expected_index == ChunkEntryCount(split_dir), "unit calls do not cover every chunk");
    return expanded;
}

// Guard against vacuous fixtures: no unhandled text in any block body.
void CheckBodyGuard(const std::map<std::string, std::string>& bodies) {
    for (const auto& [name, body] : bodies) {
        Check(body.find("// Unhandled") == std::string::npos, "unhandled instruction in " + name);
        Check(body.find("cpu.r[") != std::string::npos, "no register write in " + name);
    }
}

// ---- Tests ------------------------------------------------------------------

// Test 1: each unit fits the budget unless it holds one block; the 71-instruction block is alone.
void BoundedUnitsRespectBudget(const fs::path& root) {
    const auto dir = root / "bounded";
    Reset(dir);
    arm11::BinaryLoader image;
    const auto code = WordsToBytes(BoundedWords());
    LoadImage(dir / "input", code, 4, image);
    const auto cfg = ArmCfg(code, kBase);
    Check(EmitUnits(MainPath(dir / "out"), cfg, &image, kBudget), "EmitTranslatedUnits returned false");
    Check(cfg.size() >= 2, "fixture has too few blocks");

    const auto units = UnitKeys(dir / "out");
    Check(units.size() >= 2, "expected at least two units");
    bool oversized_alone = false;
    std::size_t total_keys = 0;
    std::size_t total_instructions = 0;
    for (const auto& keys : units) {
        const auto sum = UnitInstructions(cfg, keys);
        total_keys += keys.size();
        total_instructions += sum;
        if (keys.size() == 1 && sum > kBudget) {
            oversized_alone = true;
        } else {
            Check(sum <= kBudget, "unit with " + std::to_string(keys.size()) + " blocks exceeds budget");
        }
    }
    Check(oversized_alone, "71-instruction block did not get its own unit");
    Check(total_instructions >= 600, "fixture has fewer than 600 instructions");
    Check(total_keys == cfg.size(), "units do not cover every block exactly once");
}

// Test 2: greedy packing; the first block of the next unit would overflow the current one.
void UnitsAreGreedy(const fs::path& root) {
    const auto dir = root / "greedy";
    Reset(dir);
    arm11::BinaryLoader image;
    const auto code = WordsToBytes(BoundedWords());
    LoadImage(dir / "input", code, 4, image);
    const auto cfg = ArmCfg(code, kBase);
    Check(EmitUnits(MainPath(dir / "out"), cfg, &image, kBudget), "EmitTranslatedUnits returned false");

    const auto units = UnitKeys(dir / "out");
    Check(units.size() >= 2, "expected at least two units");
    for (std::size_t i = 0; i + 1 < units.size(); ++i) {
        Check(!units[i].empty() && !units[i + 1].empty(), "empty unit");
        const auto next_first = InstructionCount(cfg, units[i + 1].front());
        Check(UnitInstructions(cfg, units[i]) + next_first > kBudget,
              "unit " + std::to_string(i) + " is under-filled");
    }
}

// Test 3: the same CFG in any insertion order, with any bucket layout, gives identical files.
void DeterministicAcrossInsertionOrder(const fs::path& root) {
    const auto dir = root / "determinism";
    Reset(dir);
    arm11::BinaryLoader image;
    const auto code = WordsToBytes(BoundedWords());
    LoadImage(dir / "input", code, 4, image);
    const auto forward = ArmCfg(code, kBase);

    const std::vector<Cfg> variants = {forward, ReversedCfg(forward, 0), ReversedCfg(forward, 7),
                                       ReversedCfg(forward, 4096), ArmCfg(code, kBase)};
    std::map<std::string, std::string> first;
    for (std::size_t run = 0; run < variants.size(); ++run) {
        const auto out = dir / ("run_" + std::to_string(run));
        Reset(out);
        Check(EmitUnits(MainPath(out), variants[run], &image, kBudget), "EmitTranslatedUnits returned false");
        const auto files = Snapshot(out);
        if (run == 0) {
            first = files;
            Check(first.size() >= 3, "expected main file plus at least two chunks");
        } else {
            Check(files == first, "run " + std::to_string(run) + " differs from run 0");
        }
    }
}

// Test 4: expanded split registrations equal the unsplit RegisterBlock sequence, in order.
void RegistrationOrderMatchesUnsplit(const fs::path& root) {
    const auto dir = root / "registration";
    Reset(dir);
    arm11::BinaryLoader image;
    const auto code = WordsToBytes(BoundedWords());
    LoadImage(dir / "input", code, 4, image);
    const auto cfg = ArmCfg(code, kBase);

    const auto unsplit_dir = dir / "unsplit";
    const auto split_dir = dir / "split";
    Reset(unsplit_dir);
    Reset(split_dir);
    arm11::CppEmitter emitter;
    Check(emitter.EmitTranslatedFile((unsplit_dir / "translated_game.cpp").string(), cfg, &image),
          "EmitTranslatedFile returned false");
    Check(EmitUnits(MainPath(split_dir), cfg, &image, kBudget), "EmitTranslatedUnits returned false");

    const auto unsplit = ParseRegistrations(ReadText(unsplit_dir / "translated_game.cpp"));
    Check(unsplit.size() == cfg.size(), "unsplit registration count differs from CFG");
    Check(ExpandedSplitRegistrations(split_dir) == unsplit, "split registration order differs");
}

// Test 5: every block function text is identical to the unsplit text, through its closing brace.
void BlockBodiesIdentical(const fs::path& root) {
    const auto dir = root / "bodies";
    Reset(dir);
    arm11::BinaryLoader image;
    const auto code = WordsToBytes(BoundedWords());
    LoadImage(dir / "input", code, 4, image);
    const auto cfg = ArmCfg(code, kBase);

    const auto unsplit_dir = dir / "unsplit";
    const auto split_dir = dir / "split";
    Reset(unsplit_dir);
    Reset(split_dir);
    arm11::CppEmitter emitter;
    Check(emitter.EmitTranslatedFile((unsplit_dir / "translated_game.cpp").string(), cfg, &image),
          "EmitTranslatedFile returned false");
    Check(EmitUnits(MainPath(split_dir), cfg, &image, kBudget), "EmitTranslatedUnits returned false");

    const auto unsplit = ExtractBodies(ReadText(unsplit_dir / "translated_game.cpp"));
    CheckBodyGuard(unsplit);
    Check(unsplit.size() == cfg.size(), "unsplit body count differs from CFG");

    std::map<std::string, std::string> split;
    for (std::size_t i = 0; i < ChunkEntryCount(split_dir); ++i) {
        for (const auto& [name, body] : ExtractBodies(ReadText(ChunkPath(split_dir, i)))) {
            Check(split.emplace(name, body).second, "block " + name + " appears in two chunks");
        }
    }
    Check(split.size() == unsplit.size(), "split body count differs from unsplit");
    for (const auto& [name, body] : unsplit) {
        const auto found = split.find(name);
        Check(found != split.end(), "block " + name + " missing from chunks");
        Check(found->second == body, "block " + name + " body differs");
    }
}

// Test 6: the dispatcher without registration lines matches unsplit for every entry kind.
void EntryInitUnchanged(const fs::path& root) {
    const auto dir = root / "entry";
    Reset(dir);
    arm11::BinaryLoader arm_image;
    const auto arm_code = WordsToBytes(BoundedWords());
    LoadImage(dir / "arm_input", arm_code, 4, arm_image);
    arm11::BinaryLoader thumb_image;
    const auto thumb_code = ThumbCode();
    LoadImage(dir / "thumb_input", thumb_code, 2, thumb_image);

    struct EntryCase {
        std::string name;
        Cfg cfg;
        const arm11::BinaryLoader* image;
        std::string thumb_line;
    };
    const std::vector<EntryCase> cases = {
        {"arm_image", ArmCfg(arm_code, kBase), &arm_image, "    engine.GetCPU().thumb = false;\n"},
        {"thumb_image", ThumbCfgFromCode(thumb_code, kBase), &thumb_image, "    engine.GetCPU().thumb = true;\n"},
        {"no_image_arm", ArmCfg(arm_code, kBase), nullptr, "    engine.GetCPU().thumb = false;\n"},
        {"no_image_thumb", ThumbCfgFromCode(thumb_code, kBase), nullptr, "    engine.GetCPU().thumb = true;\n"},
    };
    for (const auto& entry_case : cases) {
        const auto unsplit_dir = dir / (entry_case.name + "_unsplit");
        const auto split_dir = dir / (entry_case.name + "_split");
        Reset(unsplit_dir);
        Reset(split_dir);
        arm11::CppEmitter emitter;
        Check(emitter.EmitTranslatedFile((unsplit_dir / "translated_game.cpp").string(), entry_case.cfg,
                                         entry_case.image),
              entry_case.name + ": EmitTranslatedFile returned false");
        Check(EmitUnits(MainPath(split_dir), entry_case.cfg, entry_case.image, 16u),
              entry_case.name + ": EmitTranslatedUnits returned false");
        const auto unsplit_tail = DispatcherTail(ReadText(unsplit_dir / "translated_game.cpp"));
        const auto split_tail = DispatcherTail(ReadText(MainPath(split_dir)));
        Check(unsplit_tail.find(entry_case.thumb_line) != std::string::npos,
              entry_case.name + ": expected thumb line missing from unsplit dispatcher");
        Check(split_tail == unsplit_tail, entry_case.name + ": dispatcher text differs from unsplit");
    }
}

// Test 7: only units that contain an SVC include svc_handler.hpp.
void SvcIncludePerUnit(const fs::path& root) {
    const auto dir = root / "svc";
    Reset(dir);
    arm11::BinaryLoader image;
    auto words = BoundedWords();
    words[kSvcIndex] = 0xEF000042u;
    const auto code = WordsToBytes(words);
    LoadImage(dir / "input", code, 4, image);
    const auto cfg = ArmCfg(code, kBase);
    const auto out = dir / "out";
    Reset(out);
    Check(EmitUnits(MainPath(out), cfg, &image, kBudget), "EmitTranslatedUnits returned false");

    const auto chunk_count = ChunkEntryCount(out);
    Check(chunk_count >= 2, "expected at least two units");
    std::vector<std::size_t> with_include;
    std::vector<std::size_t> with_call;
    for (std::size_t i = 0; i < chunk_count; ++i) {
        const auto text = ReadText(ChunkPath(out, i));
        if (text.find("#include \"svc_handler.hpp\"") != std::string::npos) with_include.push_back(i);
        if (text.find("wiiu::SvcHandler::Execute") != std::string::npos) with_call.push_back(i);
    }
    Check(!with_call.empty(), "no SVC call emitted");
    Check(with_include == with_call, "svc_handler.hpp include does not match SVC units");
    Check(with_call.size() < chunk_count, "every unit includes the SVC handler");
    Check(ReadText(MainPath(out)).find("svc_handler.hpp") == std::string::npos,
          "main unit includes svc_handler.hpp");
}

// Test 8: an empty CFG without an image gives one main file and no units.
void EmptyCfgNoImage(const fs::path& root) {
    const auto dir = root / "empty";
    Reset(dir);
    Check(EmitUnits(MainPath(dir), Cfg{}, nullptr, kBudget), "EmitTranslatedUnits returned false");
    const auto main_text = ReadText(MainPath(dir));
    Check(main_text.find("void InitDispatcher(") != std::string::npos, "main unit lacks InitDispatcher");
    Check(main_text.find("RegisterTranslatedBlocks") == std::string::npos, "main unit calls a unit");
    Check(ChunkEntryCount(dir) == 0, "chunk files written for an empty CFG");
}

// Test 9: budget 0 and more than 10000 units are rejected; no file is written.
void ZeroBudgetAndTooManyUnitsRejected(const fs::path& root) {
    arm11::BinaryLoader image;
    const auto code = WordsToBytes(BoundedWords());
    LoadImage(root / "budget_input", code, 4, image);
    const auto cfg = ArmCfg(code, kBase);

    const auto ok_dir = root / "budget_ok";
    Reset(ok_dir);
    Check(EmitUnits(MainPath(ok_dir), cfg, &image, kBudget), "control: valid budget rejected");
    Check(ChunkEntryCount(ok_dir) >= 2, "control: no chunks written");

    const auto zero_dir = root / "budget_zero";
    Reset(zero_dir);
    Check(!EmitUnits(MainPath(zero_dir), cfg, &image, 0u), "budget 0 accepted");
    Check(EntryCount(zero_dir) == 0, "budget 0 wrote files");

    const auto many_dir = root / "budget_many";
    Reset(many_dir);
    const auto small_dir = root / "budget_small";
    Reset(small_dir);
    Check(EmitUnits(MainPath(small_dir), SingleInstructionBlocks(3u), nullptr, 1u), "control: 3 units rejected");
    const auto many = SingleInstructionBlocks(10001u);
    Check(!EmitUnits(MainPath(many_dir), many, nullptr, 1u), "10001 units accepted");
    Check(EntryCount(many_dir) == 0, "too many units wrote files");
}

// Test 10: a missing image entry is rejected before any file is created.
void MissingEntryRejectedBeforeIO(const fs::path& root) {
    arm11::BinaryLoader image;
    const Bytes entry_code = {0x00, 0x00, 0xA0, 0xE1, 0x00, 0x00, 0xA0, 0xE1};
    LoadImage(root / "missing_input", entry_code, 4, image);
    const auto valid_dir = root / "entry_valid";
    Reset(valid_dir);
    Check(EmitUnits(MainPath(valid_dir), ArmCfg(entry_code, kBase), &image, kBudget), "control: valid entry rejected");
    const auto cfg = ArmCfg({0x00, 0x00, 0xA0, 0xE1}, kBase + 4u);
    const auto dir = root / "missing_entry";
    Reset(dir);
    Check(!EmitUnits(MainPath(dir), cfg, &image, kBudget), "missing entry accepted");
    Check(EntryCount(dir) == 0, "missing entry created files");
}

std::string HexKey(std::uint32_t key) {
    std::ostringstream text;
    text << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << key;
    return text.str();
}

Bytes BytesOf(const std::string& text) { return Bytes(text.begin(), text.end()); }

// ---- Publisher, progress and failure tests (plan tests 10-18) -------------------

void ExpectFailedAt(const std::string& log, const std::string& phase) {
    Check(log.find("FAILED phase=" + phase) != std::string::npos,
          "missing FAILED phase=" + phase + " in progress output: " + log);
}

bool EmitUnitsLogged(const fs::path& main_path, const Cfg& cfg, const arm11::BinaryLoader* image,
                     std::uint32_t budget, std::ostringstream& log) {
    return EmitUnits(main_path, cfg, image, budget, arm11::MakeLineProgressSink(log));
}

std::size_t CountWithSuffix(const fs::path& dir, const std::string& suffix) {
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        const auto name = entry.path().filename().string();
        if (name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            ++count;
        }
    }
    return count;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) lines.push_back(line);
    return lines;
}

// Phase name after "phase=" in a progress line; empty when absent.
std::string PhaseOf(const std::string& line) {
    const auto begin = line.find("phase=");
    if (begin == std::string::npos) return {};
    const auto start = begin + 6;
    const auto end = line.find(' ', start);
    return line.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// Completed count: the "N/M" token after the phase name.
std::size_t CompletedOf(const std::string& line) {
    const auto start = line.find(' ', line.find("phase=")) + 1;
    return static_cast<std::size_t>(std::stoull(line.substr(start)));
}

// Every byte printable ASCII or newline; every line is a progress line.
void CheckLineOnly(const std::string& text) {
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        Check(c == '\n' || (byte >= 0x20 && byte <= 0x7E), "non-printable byte in progress output");
    }
    for (const auto& line : SplitLines(text)) {
        Check(line.rfind("[3DS2U] progress", 0) == 0, "progress line has wrong prefix: " + line);
    }
}

// Test 10: only regular files named <stem>_blocks_<digits>.cpp outside the new set are removed.
void StaleChunksRemovedStrictly(const fs::path& root) {
    const auto dir = root / "stale_strict";
    Reset(dir);
    const auto stale = dir / "translated_game_blocks_0009.cpp";
    const std::vector<fs::path> near_misses = {
        dir / "translated_game_blocks_12x.cpp",
        dir / "translated_game_blocks_0003.cpp.bak",
        dir / "other_blocks_0000.cpp",
        dir / "translated_game_notes.cpp",
    };
    SaveBytes(stale, Bytes{'s'});
    for (const auto& path : near_misses) SaveBytes(path, Bytes{'k'});
    std::ostringstream log;
    Check(EmitUnitsLogged(MainPath(dir), SingleInstructionBlocks(3u), nullptr, 1u, log),
          "emit failed: " + log.str());
    Check(!fs::exists(stale), "stale chunk translated_game_blocks_0009.cpp survived");
    for (const auto& path : near_misses) {
        Check(fs::exists(path) && ReadText(path) == "k", "near miss removed: " + path.filename().string());
    }
    Check(fs::exists(ChunkPath(dir, 2)), "new chunk set incomplete");
}

// Test 11: a directory named like a chunk fails the publish and is never deleted.
void StaleDirectoryFails(const fs::path& root) {
    const auto dir = root / "stale_dir";
    Reset(dir);
    const auto blocker = dir / "translated_game_blocks_0000.cpp";
    fs::create_directories(blocker);
    SaveBytes(blocker / "keep.txt", Bytes{'k'});
    std::ostringstream log;
    Check(!EmitUnitsLogged(MainPath(dir), SingleInstructionBlocks(3u), nullptr, 1u, log),
          "directory stale match accepted");
    ExpectFailedAt(log.str(), "publish");
    Check(fs::is_directory(blocker) && fs::exists(blocker / "keep.txt"), "stale directory was modified");
}

// Test 12: a directory link named like a chunk is refused and its target survives.
// The Windows junction variant needs PowerShell, which this Linux container lacks, so only the link form runs.
void StaleJunctionNotFollowed(const fs::path& root) {
    const auto dir = root / "stale_link";
    Reset(dir);
    const auto target = root / "stale_link_target";
    Reset(target);
    SaveBytes(target / "keep.txt", Bytes{'k'});
    const auto link = dir / "translated_game_blocks_0000.cpp";
    std::error_code error;
    fs::create_directory_symlink(target, link, error);
    if (error) {
        std::cout << "SKIPPED StaleJunctionNotFollowed: cannot create directory link: " << error.message() << '\n';
        return;
    }
    std::ostringstream log;
    Check(!EmitUnitsLogged(MainPath(dir), SingleInstructionBlocks(3u), nullptr, 1u, log),
          "directory link accepted");
    ExpectFailedAt(log.str(), "publish");
    Check(fs::is_symlink(link), "directory link was removed or replaced");
    Check(fs::exists(target / "keep.txt"), "link target was modified");
}

// Test 13: an unsupported Thumb instruction in a late block leaves no temp or final files.
void ThumbFailureLeavesNoPartialFiles(const fs::path& root) {
    arm11::ThumbTranslator thumb;
    const arm11::Instruction probe{0xDE00u, kBase, arm11::InstructionSet::Thumb};
    Check(thumb.TranslateInstruction(probe) == "// Unhandled Thumb Instruction",
          "probe: 0xDE00 is translated; pick another unsupported opcode");
    Bytes code;
    for (int i = 0; i < 40; ++i) Append16(code, 0x2000u);
    Append16(code, 0xDE00u);
    for (int i = 0; i < 4; ++i) Append16(code, 0x2000u);
    const auto cfg = ThumbCfgFromCode(code, kBase);
    const auto dir = root / "thumb_fail";
    Reset(dir);
    std::ostringstream log;
    Check(!EmitUnitsLogged(MainPath(dir), cfg, nullptr, 4u, log), "unsupported Thumb accepted");
    ExpectFailedAt(log.str(), "emit-units");
    Check(log.str().find("Unsupported Thumb instruction at 0x") != std::string::npos,
          "reason text missing: " + log.str());
    Check(CountWithSuffix(dir, ".cpp") == 0, "partial .cpp left behind");
    Check(CountWithSuffix(dir, ".3ds2u-tmp") == 0, "temp file left behind");
}

// Test 14: a main path under a missing directory fails in emit-units and creates nothing.
void UnwritableDirectoryFails(const fs::path& root) {
    const auto parent = root / "unwritable";
    fs::remove_all(parent);
    const auto dir = parent / "missing_parent";
    std::ostringstream log;
    arm11::CppEmitter emitter;
    Check(!emitter.EmitTranslatedUnits(MainPath(dir).string(), SingleInstructionBlocks(3u), nullptr,
                                       arm11::UnitOptions{1u}, arm11::MakeLineProgressSink(log)),
          "missing directory accepted");
    ExpectFailedAt(log.str(), "emit-units");
    Check(!fs::exists(parent), "missing directory was created");
}

// Test 16: when the main final cannot be replaced, the chunk finals this run renamed are removed.
void PublishFailureRollsBackChunks(const fs::path& root) {
    const auto dir = root / "rollback";
    Reset(dir);
    const auto main = MainPath(dir);
    fs::create_directories(main);
    SaveBytes(main / "keep.txt", Bytes{'k'});
    std::ostringstream log;
    Check(!EmitUnitsLogged(main, SingleInstructionBlocks(3u), nullptr, 1u, log),
          "publish over a directory accepted");
    ExpectFailedAt(log.str(), "publish");
    for (std::size_t i = 0; i < 3; ++i) {
        Check(!fs::exists(ChunkPath(dir, i)), "chunk final survived rollback: " + std::to_string(i));
    }
    Check(fs::is_directory(main) && fs::exists(main / "keep.txt"), "main directory was modified");
    Check(CountWithSuffix(dir, ".3ds2u-tmp") == 0, "temp file left behind");
}

// Test 17: progress output is printable ASCII lines only, also for a crafted path in the reason.
void ProgressIsLineOnly(const fs::path& root) {
    const auto ok_dir = root / "progress_ok";
    Reset(ok_dir);
    std::ostringstream ok_log;
    Check(EmitUnitsLogged(MainPath(ok_dir), SingleInstructionBlocks(3u), nullptr, 1u, ok_log),
          "control emit failed");
    CheckLineOnly(ok_log.str());

    // ESC, CR and LF in the directory name end up in the publish failure reason.
    const auto bad_dir = root / std::string{"progress_bad\x1b\r\nname"};
    try {
        Reset(bad_dir);
        fs::create_directories(MainPath(bad_dir));
    } catch (const fs::filesystem_error& error) {
        // Windows rejects control characters in names; the crafted-path case needs a POSIX filesystem.
        std::cout << "SKIPPED ProgressIsLineOnly crafted path: " << error.code().message() << '\n';
        return;
    }
    std::ostringstream bad_log;
    Check(!EmitUnitsLogged(MainPath(bad_dir), SingleInstructionBlocks(3u), nullptr, 1u, bad_log),
          "crafted publish accepted");
    ExpectFailedAt(bad_log.str(), "publish");
    CheckLineOnly(bad_log.str());
}

// Test 18: fixed phase order, monotonic counts, throttled emit-units lines, one complete line last.
void ProgressMonotonicCompleteLast(const fs::path& root) {
    const auto dir = root / "progress_order";
    Reset(dir);
    std::ostringstream log;
    Check(EmitUnitsLogged(MainPath(dir), SingleInstructionBlocks(100u), nullptr, 1u, log), "emit failed");
    const auto lines = SplitLines(log.str());
    Check(!lines.empty(), "no progress output");
    Check(log.str().find("FAILED") == std::string::npos, "failure reported on success");
    const std::vector<std::string> order = {"plan-units", "emit-units", "emit-dispatcher",
                                            "verify-registration", "publish", "complete"};
    std::size_t last_index = 0;
    std::size_t emit_lines = 0;
    std::size_t completes = 0;
    std::size_t last_completed = 0;
    std::string last_phase;
    for (const auto& line : lines) {
        const auto phase = PhaseOf(line);
        const auto found = std::find(order.begin(), order.end(), phase);
        Check(found != order.end(), "unknown phase in: " + line);
        const auto index = static_cast<std::size_t>(found - order.begin());
        Check(index >= last_index, "phase out of order at: " + line);
        if (phase != last_phase) last_completed = 0;
        const auto completed = CompletedOf(line);
        Check(completed >= last_completed, "completed count decreased at: " + line);
        last_completed = completed;
        last_phase = phase;
        last_index = index;
        if (phase == "emit-units") ++emit_lines;
        if (phase == "complete") {
            ++completes;
            Check(line == lines.back(), "complete is not the last line");
        }
    }
    Check(completes == 1, "expected exactly one complete line");
    Check(emit_lines >= 1 && emit_lines <= 20, "emit-units lines: " + std::to_string(emit_lines));

    std::ostringstream failed;
    Check(!EmitUnitsLogged(root / "progress_zero" / "translated_game.cpp", SingleInstructionBlocks(3u),
                           nullptr, 0u, failed),
          "budget 0 accepted");
    Check(failed.str().find("phase=complete") == std::string::npos, "complete printed on failure");
}

// Extra test 10b: identical output leaves every final file's mtime untouched (write-if-changed).
void UnchangedOutputKeepsMtime(const fs::path& root) {
    const auto dir = root / "mtime";
    Reset(dir);
    std::ostringstream first;
    Check(EmitUnitsLogged(MainPath(dir), SingleInstructionBlocks(3u), nullptr, 1u, first), "first emit failed");
    const auto stamp = fs::file_time_type::clock::now() - std::chrono::hours{24};
    // Read the stamp back: the bind-mounted filesystem may round timestamps.
    std::map<std::string, fs::file_time_type> before;
    for (const auto& entry : fs::directory_iterator(dir)) {
        fs::last_write_time(entry.path(), stamp);
        before.emplace(entry.path().filename().string(), fs::last_write_time(entry.path()));
    }
    std::ostringstream second;
    Check(EmitUnitsLogged(MainPath(dir), SingleInstructionBlocks(3u), nullptr, 1u, second), "second emit failed");
    Check(before.size() >= 2, "expected main plus chunks");
    for (const auto& [name, time] : before) {
        Check(fs::last_write_time(dir / name) == time, "identical output rewrote " + name);
    }
}

// Extra test 10c: the verifier rejects a main staged file with fewer unit calls than chunks.
void VerifyRejectsCountMismatch(const fs::path& root) {
    const auto dir = root / "verify_count";
    Reset(dir);
    arm11::UnitSetFiles files;
    files.main_path = MainPath(dir).string();
    files.chunk_stem = "translated_game";
    files.chunk_paths = {ChunkPath(dir, 0).string(), ChunkPath(dir, 1).string()};
    files.expected_keys = {{kBase}, {kBase + 4u}};
    const auto chunk_text = [](std::uint32_t key, const std::string& name) {
        return "void RegisterTranslatedBlocks" + name + "(wiiu::WiiUEngine& engine) {\n"
               "    engine.RegisterBlock(0x" + HexKey(key) + "u, &arm11::block_" + HexKey(key) + ");\n}\n";
    };
    SaveBytes(arm11::TempPathFor(files.chunk_paths[0]), BytesOf(chunk_text(kBase, "0000")));
    SaveBytes(arm11::TempPathFor(files.chunk_paths[1]), BytesOf(chunk_text(kBase + 4u, "0001")));
    SaveBytes(arm11::TempPathFor(files.main_path),
              BytesOf("void InitDispatcher(wiiu::WiiUEngine& engine) {\n"
                      "    arm11::RegisterTranslatedBlocks0000(engine);\n}\n"));
    bool rejected = false;
    std::string reason;
    try {
        arm11::VerifyStagedFiles(files);
    } catch (const std::runtime_error& error) {
        rejected = true;
        reason = error.what();
    }
    Check(rejected, "main with 1 unit call accepted for 2 units");
    Check(reason.find("count") != std::string::npos, "wrong rejection reason: " + reason);
}

// Hostile staging tests (CWE-59 / CWE-377). The link forms need link privileges; a missing privilege skips.
struct SkipTest : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Throws SkipTest when links cannot be created, so main reports SKIP instead of a vacuous PASS.
void MakeFileLink(const fs::path& target, const fs::path& link) {
    std::error_code error;
    fs::create_symlink(target, link, error);
    if (error) throw SkipTest("cannot create file link: " + error.message());
}

fs::path StagePath(const fs::path& dir, std::size_t chunk) {
    return arm11::TempPathFor(ChunkPath(dir, chunk).string());
}

// Test 19: a link planted at a staging name is not followed; its target is not overwritten.
void StagingLinkNotFollowed(const fs::path& root) {
    const auto dir = root / "stage_link";
    Reset(dir);
    const auto victim = root / "stage_link_victim.txt";
    SaveBytes(victim, BytesOf("precious"));
    MakeFileLink(victim, StagePath(dir, 0));
    std::ostringstream log;
    Check(!EmitUnitsLogged(MainPath(dir), SingleInstructionBlocks(3u), nullptr, 1u, log), "staging link accepted");
    ExpectFailedAt(log.str(), "emit-units");
    Check(ReadText(victim) == "precious", "link target was overwritten");
    Check(fs::is_symlink(StagePath(dir, 0)), "planted link was removed or replaced");
    Check(!fs::exists(ChunkPath(dir, 0)) && !fs::exists(MainPath(dir)), "final published after refusal");
}

// Test 20: a dangling link at a staging name does not create its target.
void StagingDanglingLinkNotCreated(const fs::path& root) {
    const auto dir = root / "stage_dangling";
    Reset(dir);
    const auto target = root / "stage_dangling_target.txt";
    fs::remove(target);
    MakeFileLink(target, StagePath(dir, 0));
    std::ostringstream log;
    Check(!EmitUnitsLogged(MainPath(dir), SingleInstructionBlocks(3u), nullptr, 1u, log), "dangling link accepted");
    Check(!fs::exists(target), "dangling link target was created");
}

// Test 21: a directory at a staging name is refused and kept.
void StagingDirectoryRefused(const fs::path& root) {
    const auto dir = root / "stage_dir";
    Reset(dir);
    fs::create_directories(StagePath(dir, 1));
    SaveBytes(StagePath(dir, 1) / "keep.txt", Bytes{'k'});
    std::ostringstream log;
    Check(!EmitUnitsLogged(MainPath(dir), SingleInstructionBlocks(3u), nullptr, 1u, log), "staging directory accepted");
    ExpectFailedAt(log.str(), "emit-units");
    Check(fs::exists(StagePath(dir, 1) / "keep.txt"), "staging directory was modified");
}

// Test 22: a leftover regular staging file of a crashed run is replaced and the run succeeds.
void StaleRegularStagingReplaced(const fs::path& root) {
    const auto dir = root / "stage_regular";
    Reset(dir);
    SaveBytes(StagePath(dir, 0), BytesOf("leftover from a crash"));
    std::ostringstream log;
    Check(EmitUnitsLogged(MainPath(dir), SingleInstructionBlocks(3u), nullptr, 1u, log), "leftover blocked the run");
    Check(CountWithSuffix(dir, ".3ds2u-tmp") == 0, "temp file left behind");
    Check(ReadText(ChunkPath(dir, 0)).find("leftover") == std::string::npos, "leftover bytes were published");
}

// Test 23: a refused final or stale match stops the run before any staged file is written.
void RefusalPrecedesStagedWrites(const fs::path& root) {
    const auto dir = root / "stage_order";
    Reset(dir);
    const auto sentinel = StagePath(dir, 0);
    SaveBytes(sentinel, BytesOf("sentinel"));
    fs::create_directories(dir / "translated_game_blocks_0007.cpp");
    std::ostringstream log;
    Check(!EmitUnitsLogged(MainPath(dir), SingleInstructionBlocks(3u), nullptr, 1u, log), "stale directory accepted");
    ExpectFailedAt(log.str(), "publish");
    Check(ReadText(sentinel) == "sentinel", "a staged file was written before the refusal check");
    Check(!fs::exists(StagePath(dir, 1)) && !fs::exists(arm11::TempPathFor(MainPath(dir).string())),
          "later staged files exist after refusal");
}

// Test 24: a link at a final name is refused, its target is not replaced.
void FinalFileLinkRefused(const fs::path& root) {
    const auto dir = root / "final_link";
    Reset(dir);
    const auto victim = root / "final_link_victim.txt";
    SaveBytes(victim, BytesOf("precious"));
    MakeFileLink(victim, ChunkPath(dir, 1));
    std::ostringstream log;
    Check(!EmitUnitsLogged(MainPath(dir), SingleInstructionBlocks(3u), nullptr, 1u, log), "final link accepted");
    ExpectFailedAt(log.str(), "publish");
    Check(ReadText(victim) == "precious", "final link target was overwritten");
    Check(fs::is_symlink(ChunkPath(dir, 1)), "final link was replaced");
}

// Test 25: a staged file swapped for a link after verification is refused at publish.
void StagedSwapToLinkRefusedAtPublish(const fs::path& root) {
    const auto dir = root / "swap_link";
    Reset(dir);
    arm11::UnitSetFiles files;
    files.main_path = MainPath(dir).string();
    files.chunk_stem = "translated_game";
    files.chunk_paths = {ChunkPath(dir, 0).string()};
    files.expected_keys = {{kBase}};
    SaveBytes(arm11::TempPathFor(files.chunk_paths[0]),
              BytesOf("void RegisterTranslatedBlocks0000(wiiu::WiiUEngine& engine) {\n"
                      "    engine.RegisterBlock(0x" + HexKey(kBase) + "u, &arm11::block_" + HexKey(kBase) + ");\n}\n"));
    SaveBytes(arm11::TempPathFor(files.main_path),
              BytesOf("void InitDispatcher(wiiu::WiiUEngine& engine) {\n"
                      "    arm11::RegisterTranslatedBlocks0000(engine);\n}\n"));
    arm11::VerifyStagedFiles(files);
    const auto attacker = root / "swap_link_attacker.txt";
    SaveBytes(attacker, BytesOf("attacker content"));
    fs::remove(arm11::TempPathFor(files.chunk_paths[0]));
    MakeFileLink(attacker, arm11::TempPathFor(files.chunk_paths[0]));
    bool refused = false;
    try {
        arm11::PublishStagedFiles(files);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    Check(refused, "swapped staged file was published");
    Check(!fs::exists(files.chunk_paths[0]), "chunk final exists after refused publish");
    Check(ReadText(attacker) == "attacker content", "attacker file modified");
    bool verify_refused = false;
    try {
        arm11::VerifyStagedFiles(files);
    } catch (const std::runtime_error&) {
        verify_refused = true;
    }
    Check(verify_refused, "verify read through a staged link");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: split_units_tests <work-dir>\n";
        return 2;
    }
    const fs::path root = argv[1];
    fs::create_directories(root);
    using TestFn = void (*)(const fs::path&);
    const std::vector<std::pair<std::string, TestFn>> tests = {
        {"BoundedUnitsRespectBudget", BoundedUnitsRespectBudget},
        {"UnitsAreGreedy", UnitsAreGreedy},
        {"DeterministicAcrossInsertionOrder", DeterministicAcrossInsertionOrder},
        {"RegistrationOrderMatchesUnsplit", RegistrationOrderMatchesUnsplit},
        {"BlockBodiesIdentical", BlockBodiesIdentical},
        {"EntryInitUnchanged", EntryInitUnchanged},
        {"SvcIncludePerUnit", SvcIncludePerUnit},
        {"EmptyCfgNoImage", EmptyCfgNoImage},
        {"ZeroBudgetAndTooManyUnitsRejected", ZeroBudgetAndTooManyUnitsRejected},
        {"MissingEntryRejectedBeforeIO", MissingEntryRejectedBeforeIO},
        {"StaleChunksRemovedStrictly", StaleChunksRemovedStrictly},
        {"StaleDirectoryFails", StaleDirectoryFails},
        {"StaleJunctionNotFollowed", StaleJunctionNotFollowed},
        {"ThumbFailureLeavesNoPartialFiles", ThumbFailureLeavesNoPartialFiles},
        {"UnwritableDirectoryFails", UnwritableDirectoryFails},
        {"UnchangedOutputKeepsMtime", UnchangedOutputKeepsMtime},
        {"PublishFailureRollsBackChunks", PublishFailureRollsBackChunks},
        {"VerifyRejectsCountMismatch", VerifyRejectsCountMismatch},
        {"ProgressIsLineOnly", ProgressIsLineOnly},
        {"ProgressMonotonicCompleteLast", ProgressMonotonicCompleteLast},
        {"StagingLinkNotFollowed", StagingLinkNotFollowed},
        {"StagingDanglingLinkNotCreated", StagingDanglingLinkNotCreated},
        {"StagingDirectoryRefused", StagingDirectoryRefused},
        {"StaleRegularStagingReplaced", StaleRegularStagingReplaced},
        {"RefusalPrecedesStagedWrites", RefusalPrecedesStagedWrites},
        {"FinalFileLinkRefused", FinalFileLinkRefused},
        {"StagedSwapToLinkRefusedAtPublish", StagedSwapToLinkRefusedAtPublish},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test(root);
            std::cout << "PASS " << name << '\n';
        } catch (const SkipTest& skip) {
            std::cout << "SKIP " << name << ": " << skip.what() << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << (failures == 0 ? "Split unit tests passed.\n" : "Split unit tests FAILED.\n");
    return failures == 0 ? 0 : 1;
}
