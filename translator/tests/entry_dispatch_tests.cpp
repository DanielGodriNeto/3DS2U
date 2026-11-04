#include "binary_loader.hpp"
#include "cfg_builder.hpp"
#include "cpp_emitter.hpp"
#include "decoder.hpp"
#include "thumb_decoder.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// Synthetic entry-point regression: the emitted dispatcher must start at the
// image entry parsed by BinaryLoader, in the mode of its translated block.
namespace {

void Check(bool value) {
    if (!value) throw std::runtime_error("Entry dispatch regression");
}

void Put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (i * 8));
}

void Save(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file;
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.open(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
}

// Decrypted ExHeader with one executable section at 0x00100000.
std::vector<std::uint8_t> MakeExHeader(std::uint32_t text_size) {
    std::vector<std::uint8_t> header(0x400, 0);
    Put32(header, 0x10, 0x00100000u);
    Put32(header, 0x14, 1);
    Put32(header, 0x18, text_size);
    return header;
}

std::string ReadAll(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    Check(file.is_open());
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

std::unordered_map<std::uint32_t, arm11::BasicBlock> BuildArmCfg(
    const std::vector<std::uint8_t>& code, std::uint32_t base) {
    arm11::Decoder decoder;
    arm11::CFGBuilder builder;
    return builder.BuildBlocks(decoder.DecodeBuffer(code, base));
}

std::unordered_map<std::uint32_t, arm11::BasicBlock> BuildThumbCfg(
    const std::vector<std::uint8_t>& code, std::uint32_t base) {
    arm11::ThumbTranslator thumb;
    arm11::CFGBuilder builder;
    return builder.BuildBlocks(thumb.DecodeBuffer(code, base));
}

// Two MOV r0, r0 words; the ExHeader entry is 0x00100000.
std::vector<std::uint8_t> ArmWords() {
    return {0x00, 0x00, 0xA0, 0xE1, 0x00, 0x00, 0xA0, 0xE1};
}

// Two MOVS r0, #0 halfwords for a Thumb-aligned ExHeader entry.
std::vector<std::uint8_t> ThumbHalfwords() {
    return {0x00, 0x20, 0x00, 0x20};
}

} // namespace

int main(int argc, char** argv) {
    Check(argc == 2);
    const std::filesystem::path root = argv[1];
    std::filesystem::create_directories(root);
    const auto code_path = (root / "entry.code").string();
    const auto header_path = (root / "entry.exheader").string();
    const auto out_path = (root / "entry_arm.cpp").string();

    // ARM image: entry 0x00100000 must be registered and assigned to r[15].
    Save(code_path, ArmWords());
    Save(header_path, MakeExHeader(8));
    arm11::BinaryLoader arm_image;
    Check(arm_image.LoadFile(code_path, header_path, 4));
    Check(arm_image.GetEntryPoint() == 0x00100000u);
    const auto arm_cfg = BuildArmCfg(ArmWords(), 0x00100000u);
    Check(arm_cfg.contains(0x00100000u));
    {
        arm11::CppEmitter emitter;
        Check(emitter.EmitTranslatedFile(out_path, arm_cfg, &arm_image));
        const auto source = ReadAll(out_path);
        Check(Contains(source, "engine.RegisterBlock(0x00100000u, &arm11::block_00100000);"));
        Check(Contains(source, "engine.GetCPU().r[15] = 0x00100000u;"));
        Check(Contains(source, "engine.GetCPU().thumb = false;"));
        Check(!Contains(source, "0x08000000u"));
    }

    // Thumb image: the entry block is tagged 0x00100001 and sets thumb mode.
    const auto thumb_code_path = (root / "entry_thumb.code").string();
    const auto thumb_header_path = (root / "entry_thumb.exheader").string();
    const auto thumb_out_path = (root / "entry_thumb.cpp").string();
    Save(thumb_code_path, ThumbHalfwords());
    Save(thumb_header_path, MakeExHeader(4));
    arm11::BinaryLoader thumb_image;
    Check(thumb_image.LoadFile(thumb_code_path, thumb_header_path, 2));
    const auto thumb_cfg = BuildThumbCfg(ThumbHalfwords(), 0x00100000u);
    Check(thumb_cfg.contains(0x00100001u));
    {
        arm11::CppEmitter emitter;
        Check(emitter.EmitTranslatedFile(thumb_out_path, thumb_cfg, &thumb_image));
        const auto source = ReadAll(thumb_out_path);
        Check(Contains(source, "engine.RegisterBlock(0x00100001u, &arm11::thumb_block_00100000);"));
        Check(Contains(source, "engine.GetCPU().r[15] = 0x00100000u;"));
        Check(Contains(source, "engine.GetCPU().thumb = true;"));
    }

    // Raw input has no ExHeader; its entry is the FCRAM base.
    const auto raw_code_path = (root / "raw.code").string();
    const auto raw_out_path = (root / "raw.cpp").string();
    Save(raw_code_path, ArmWords());
    arm11::BinaryLoader raw_image;
    Check(raw_image.LoadFile(raw_code_path));
    Check(raw_image.GetEntryPoint() == 0x08000000u);
    {
        arm11::CppEmitter emitter;
        Check(emitter.EmitTranslatedFile(raw_out_path, BuildArmCfg(ArmWords(), 0x08000000u), &raw_image));
        Check(Contains(ReadAll(raw_out_path), "engine.GetCPU().r[15] = 0x08000000u;"));
    }

    // Missing entry block: only the second word is translated, so the emitter must
    // reject the image before opening the output file.
    const auto missing_out_path = (root / "entry_missing.cpp").string();
    std::filesystem::remove(missing_out_path);
    const auto missing_cfg = BuildArmCfg({0x00, 0x00, 0xA0, 0xE1}, 0x00100004u);
    Check(!missing_cfg.contains(0x00100000u));
    {
        arm11::CppEmitter emitter;
        Check(!emitter.EmitTranslatedFile(missing_out_path, missing_cfg, &arm_image));
        Check(!std::filesystem::exists(missing_out_path));
    }

    // Ambiguous entry: ARM and Thumb blocks both claim 0x00100000 and must be rejected.
    auto ambiguous_cfg = BuildArmCfg(ArmWords(), 0x00100000u);
    for (const auto& [key, block] : BuildThumbCfg(ThumbHalfwords(), 0x00100000u)) {
        ambiguous_cfg.emplace(key, block);
    }
    const auto ambiguous_out_path = (root / "entry_ambiguous.cpp").string();
    std::filesystem::remove(ambiguous_out_path);
    {
        arm11::CppEmitter emitter;
        Check(!emitter.EmitTranslatedFile(ambiguous_out_path, ambiguous_cfg, &arm_image));
        Check(!std::filesystem::exists(ambiguous_out_path));
    }

    std::cout << "Entry point dispatch emission checks passed.\n";
    return 0;
}
