#include "binary_loader.hpp"
#include "decoder.hpp"
#include "cfg_builder.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void Check(bool value) {
    if (!value) throw std::runtime_error("ExeFS loader regression");
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
}

int main(int argc, char** argv) {
    Check(argc == 2);
    const std::filesystem::path root = argv[1];
    std::filesystem::create_directories(root);
    const auto code_path = (root / "sections.code").string();
    const auto header_path = (root / "sections.exheader").string();
    const auto bad_code_path = (root / "bad.code").string();
    const auto bad_header_path = (root / "bad.exheader").string();

    std::vector<std::uint8_t> header(0x400, 0);
    Put32(header, 0x10, 0x00100000u);
    Put32(header, 0x14, 1);
    Put32(header, 0x18, 8);
    Put32(header, 0x20, 0x00200000u);
    Put32(header, 0x24, 1);
    Put32(header, 0x28, 5);
    Put32(header, 0x30, 0x00300000u);
    Put32(header, 0x34, 1);
    Put32(header, 0x38, 3);
    Put32(header, 0x3C, 9);
    std::vector<std::uint8_t> code(0x3000, 0xAA);
    Put32(code, 0, 0xE1A00000u);
    Put32(code, 4, 0xEAFFFFFEu);
    Put32(code, 0x1000, 0xEAFFFFFEu); // ARM-like data must not become CFG instructions.
    code[0x1004] = 0x77;
    code[0x2000] = 0x11;
    code[0x2001] = 0x22;
    code[0x2002] = 0x33;
    Save(code_path, code);
    Save(header_path, header);

    arm11::BinaryLoader loader;
    Check(loader.LoadFile(code_path, header_path));
    Check(loader.HasExHeader() && loader.GetEntryPoint() == 0x00100000u);
    Check(loader.GetBssSize() == 9 && loader.GetTextBuffer().size() == 8);
    Check(loader.GetCodeBuffer().size() == 0x3000 && loader.GetMappedMemory().size() == 0x3000);
    const auto sections = loader.GetSections();
    Check(sections.size() == 3 && sections[0].executable && !sections[1].executable);
    Check(!sections[1].writable && sections[2].writable);
    Check(sections[1].file_offset == 0x1000 && sections[2].file_offset == 0x2000);
    Check(sections[1].fcram_offset == 0x1000 && sections[2].fcram_offset == 0x2000);
    Check(loader.GetInstructionAddresses().size() == 2);
    Check(loader.GetInstructionAddresses()[0] == 0x00100000u);
    Check(loader.GetInstructionAddresses()[1] == 0x00100004u);
    Check(loader.GetGuestBytes(0x00200004u, 1)[0] == 0x77);
    Check(loader.GetGuestBytes(0x00300000u, 3)[2] == 0x33);
    const auto bss = loader.GetGuestBytes(0x00300003u, 9);
    Check(std::all_of(bss.begin(), bss.end(), [](auto b) { return b == 0; }));
    const auto text_padding = loader.GetGuestBytes(0x00100008u, 32);
    Check(std::all_of(text_padding.begin(), text_padding.end(), [](auto b) { return b == 0; }));
    bool unmapped = false;
    try { (void)loader.GetGuestBytes(0x00101000u, 1); }
    catch (const std::out_of_range&) { unmapped = true; }
    Check(unmapped);
    bool crossing = false;
    try { (void)loader.GetGuestBytes(0x00100FFCu, 8); }
    catch (const std::out_of_range&) { crossing = true; }
    Check(crossing);

    const arm11::Decoder decoder;
    const arm11::CFGBuilder builder;
    const auto instructions = decoder.DecodeBuffer(loader.GetTextBuffer(), loader.GetEntryPoint());
    Check(instructions.size() == 2 && instructions.front().address == loader.GetEntryPoint());
    const auto blocks = builder.BuildBlocks(instructions);
    Check(blocks.contains(loader.GetEntryPoint()) && !blocks.contains(0x00200000u));

    auto trimmed = code;
    trimmed.resize(0x2003); // Final non-code bytes need not be ARM-aligned.
    Save(bad_code_path, trimmed);
    Check(loader.LoadFile(bad_code_path, header_path));
    auto large_header = header;
    large_header.resize(0x800);
    Save(bad_header_path, large_header);
    Check(loader.LoadFile(code_path, bad_header_path));
    auto large_bss = header;
    Put32(large_bss, 0x3C, 0x1000);
    Save(bad_header_path, large_bss);
    Check(loader.LoadFile(code_path, bad_header_path));
    Check(loader.GetSections()[2].mapped_size == 0x2000);
    const auto extended_bss = loader.GetGuestBytes(0x00300003u, 0x1000);
    Check(std::all_of(extended_bss.begin(), extended_bss.end(), [](auto b) { return b == 0; }));

    unsigned rejected = 0;
    const auto reject = [&](const std::vector<std::uint8_t>& bad_header,
                            const std::vector<std::uint8_t>& bad_code) {
        Save(bad_header_path, bad_header);
        Save(bad_code_path, bad_code);
        Check(!loader.LoadFile(bad_code_path, bad_header_path));
        Check(loader.GetCodeBuffer().empty() && loader.GetMappedMemory().empty());
        Check(loader.GetSections().empty() && loader.GetInstructionAddresses().empty());
        Check(loader.GetEntryPoint() == 0);
        ++rejected;
    };
    auto bad = header;
    bad[0xD] = 1;
    reject(bad, code);
    bad = header; Put32(bad, 0x14, 0); reject(bad, code);
    bad = header; Put32(bad, 0x18, 6); reject(bad, code);
    bad = header; Put32(bad, 0x10, 0x00100002u); reject(bad, code);
    bad = header; Put32(bad, 0x20, 0x00100000u); reject(bad, code);
    bad = header; Put32(bad, 0x30, 0xFFFFF000u); Put32(bad, 0x3C, 0x1000); reject(bad, code);
    bad = header; Put32(bad, 0x28, arm11::BinaryLoader::kFCRAMSize);
    Put32(bad, 0x24, arm11::BinaryLoader::kFCRAMSize / 0x1000); reject(bad, code);
    bad = header; bad.resize(0x40); reject(bad, code);
    auto short_code = code; short_code.resize(0x2002); reject(header, short_code);
    auto long_code = code; long_code.push_back(0); reject(header, long_code);

    const auto raw_path = (root / "raw.code").string();
    Save(raw_path, std::vector<std::uint8_t>(code.begin(), code.begin() + 8));
    Check(loader.LoadFile(raw_path));
    Check(!loader.HasExHeader() && loader.GetEntryPoint() == 0x08000000u);
    Check(loader.GetInstructionAddresses().size() == 2);
    Save(bad_code_path, {1, 2, 3});
    Check(!loader.LoadFile(bad_code_path));
    Check(loader.GetSections().empty());
    Save(bad_code_path, {0x7F, 'E', 'L', 'F'});
    Check(!loader.LoadFile(bad_code_path));
    Save(bad_code_path, {});
    Check(loader.LoadFile(bad_code_path) && loader.GetInstructionAddresses().empty());
    Check(!loader.LoadFile((root / "nonexistent.code").string()));
    std::cout << "Loader mapping/raw regressions passed, including " << rejected
              << " malformed ExHeader/payload fixtures\n";
}
