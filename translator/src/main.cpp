#include "binary_loader.hpp"
#include "decoder.hpp"
#include "progress_sink.hpp"
#include "cfg_builder.hpp"
#include "cpp_emitter.hpp"
#include "thumb_decoder.hpp"

#include <exception>
#include <iomanip>
#include <stdexcept>
#include <iostream>
#include <string>
#include <algorithm>
#include <limits>
#include <optional>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: 3ds2u_translator <input_binary.code> <output_source.cpp>\n";
        std::cerr << "       Add --exheader <decrypted.exheader> for ExeFS section mapping\n";
        std::cerr << "       Add --thumb, or repeat --thumb-range <guest_address>:<byte_size>\n";
        std::cerr << "       Add --split-instructions N for bounded translation units of at most N instructions\n";
        return 1;
    }

    const std::string input_path = argv[1];
    const std::string output_path = argv[2];
    std::cout << "[3DS2U] Starting translation\n"
              << "[3DS2U] Input:  \"" << input_path << "\"\n"
              << "[3DS2U] Output: \"" << output_path << "\"\n";

    try {
        std::string exheader_path;
        bool all_thumb = false;
        struct Range { std::uint32_t start, size; };
        std::vector<Range> ranges;
        std::optional<std::uint32_t> split_budget;
        const auto number = [](const std::string& text) {
            std::size_t used = 0;
            if (text.empty() || text.front() == '-') throw std::invalid_argument("Invalid mode-range number");
            const auto value = std::stoull(text, &used, 0);
            if (used != text.size() || value > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument("Mode-range number exceeds 32 bits");
            }
            return static_cast<std::uint32_t>(value);
        };
        for (int i = 3; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--thumb") all_thumb = true;
            else if (option == "--exheader" && i + 1 < argc && exheader_path.empty()) {
                exheader_path = argv[++i];
                if (exheader_path.empty()) throw std::invalid_argument("Empty ExHeader path");
            } else if (option == "--thumb-range" && i + 1 < argc) {
                const std::string range = argv[++i];
                const auto colon = range.find(':');
                if (colon == std::string::npos) throw std::invalid_argument("Expected Thumb address:size");
                const auto start = number(range.substr(0, colon));
                const auto size = number(range.substr(colon + 1));
                if ((start & 1u) != 0 || (size & 1u) != 0 || size == 0 ||
                    std::uint64_t{start} + size > (std::uint64_t{1} << 32)) {
                    throw std::invalid_argument("Thumb ranges require aligned nonempty halfwords");
                }
                ranges.push_back({start, size});
            } else if (option == "--split-instructions" && i + 1 < argc && !split_budget) {
                split_budget = number(argv[++i]);
                if (*split_budget == 0) throw std::invalid_argument("--split-instructions requires N >= 1");
            } else throw std::invalid_argument("Unknown, incomplete, or repeated CLI option: " + option);
        }
        if (all_thumb && !ranges.empty()) throw std::invalid_argument("Use --thumb or --thumb-range, not both");
        arm11::BinaryLoader loader;
        if (!loader.LoadFile(input_path, exheader_path, all_thumb || !ranges.empty() ? 2u : 4u)) {
            std::cerr << "[3DS2U] Translation failed: cannot load \"" << input_path << "\"\n";
            return 1;
        }

        std::cout << "[3DS2U] Layout: " << (loader.HasExHeader() ?
            "ExeFS .code with decrypted ExHeader" : "Raw code; entire input is .text (no ExHeader)") << '\n'
                  << "[3DS2U] Binary length: " << loader.GetCodeBuffer().size() << " bytes\n";
        for (const auto& section : loader.GetSections()) {
            std::cout << "[3DS2U] " << section.name << ": " << section.size
                      << " initialized bytes, " << section.mapped_size << " mapped bytes, guest VA 0x"
                      << std::hex << std::uppercase << section.virtual_address
                      << ", FCRAM backing 0x" << arm11::BinaryLoader::kFCRAMBase + section.fcram_offset
                      << std::dec << '\n';
        }
        std::cout << "[3DS2U] BSS: " << loader.GetBssSize() << " zero-initialized bytes\n"
                  << "[3DS2U] Entry point: 0x" << std::hex << std::uppercase
                  << loader.GetEntryPoint() << std::dec << '\n';
        arm11::Decoder decoder;
        arm11::ThumbTranslator thumb;
        std::vector<arm11::Instruction> instructions;
        const auto text = loader.GetTextBuffer();
        const std::uint64_t entry = loader.GetEntryPoint();
        const auto end = entry + text.size();
        std::sort(ranges.begin(), ranges.end(), [](auto a, auto b) { return a.start < b.start; });
        const auto append = [&](std::uint64_t start, std::uint64_t stop, bool t16) {
            if (start == stop) return;
            const auto bytes = text.subspan(static_cast<std::size_t>(start - entry),
                                            static_cast<std::size_t>(stop - start));
            const auto decoded = t16 ? thumb.DecodeBuffer(bytes, static_cast<std::uint32_t>(start)) :
                                       decoder.DecodeBuffer(bytes, static_cast<std::uint32_t>(start));
            instructions.insert(instructions.end(), decoded.begin(), decoded.end());
        };
        auto cursor = entry;
        if (all_thumb) append(entry, end, true);
        else {
            for (const auto& range : ranges) {
                const std::uint64_t stop = std::uint64_t{range.start} + range.size;
                if (range.start < cursor || stop > end) throw std::invalid_argument("Thumb ranges overlap or leave .text");
                append(cursor, range.start, false);
                append(range.start, stop, true);
                cursor = stop;
            }
            append(cursor, end, false);
        }
        if (instructions.empty()) throw std::runtime_error("No instructions in .text");
        std::cout << "[3DS2U] Parsed " << instructions.size() << " ARM/Thumb instructions\n";
        arm11::CFGBuilder builder;
        const auto blocks = builder.BuildBlocks(instructions);
        arm11::CppEmitter emitter;
        bool emitted = false;
        if (split_budget) {
            emitted = emitter.EmitTranslatedUnits(output_path, blocks, &loader, arm11::UnitOptions{*split_budget},
                                                  arm11::MakeLineProgressSink(std::cout));
        } else {
            emitted = emitter.EmitTranslatedFile(output_path, blocks, &loader);
        }
        if (!emitted) {
            std::cerr << "[3DS2U] Translation failed: cannot emit \"" << output_path << "\"\n";
            return 1;
        }

        std::cout << "[3DS2U] Translation complete: \"" << input_path << "\" -> \""
                  << output_path << "\"\n"
                  << "[3DS2U] Decoded " << instructions.size() << " instructions into "
                  << blocks.size() << " basic blocks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[3DS2U] Translation failed: " << error.what() << '\n';
        return 1;
    }
}
