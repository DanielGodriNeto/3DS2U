#include "binary_loader.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace arm11 {
namespace {

std::vector<std::uint8_t> ReadFile(const std::string& path, std::size_t limit) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) throw std::runtime_error("Cannot open file: " + path);
    const std::streamoff length = file.tellg();
    if (length < 0) throw std::runtime_error("Cannot determine file size: " + path);
    const auto size = static_cast<std::uintmax_t>(length);
    if (size > limit || size > std::vector<std::uint8_t>{}.max_size() ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("File exceeds supported size: " + path);
    }
    file.seekg(0, std::ios::beg);
    if (!file) throw std::runtime_error("Cannot seek in file: " + path);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        const auto count = static_cast<std::streamsize>(size);
        file.read(reinterpret_cast<char*>(bytes.data()), count);
        if (!file || file.gcount() != count) {
            throw std::runtime_error("Cannot read entire file: " + path);
        }
    }
    return bytes;
}

std::uint32_t ReadLE32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        throw std::runtime_error("Truncated ExHeader field");
    }
    return std::uint32_t{bytes[offset]} | (std::uint32_t{bytes[offset + 1]} << 8) |
           (std::uint32_t{bytes[offset + 2]} << 16) |
           (std::uint32_t{bytes[offset + 3]} << 24);
}

std::uint32_t PageSize(std::uint64_t size) {
    const auto rounded = (size + BinaryLoader::kPageSize - 1u) &
                         ~std::uint64_t{BinaryLoader::kPageSize - 1u};
    if (rounded > BinaryLoader::kFCRAMSize) {
        throw std::runtime_error("Section exceeds simulated FCRAM capacity");
    }
    return static_cast<std::uint32_t>(rounded);
}

} // namespace

bool BinaryLoader::LoadFile(const std::string& filepath, const std::string& exheader_path,
                            std::uint32_t instruction_alignment) {
    *this = BinaryLoader{};
    try {
        BinaryLoader image;
        image.code_ = ReadFile(filepath, kFCRAMSize);
        std::vector<std::uint8_t> exheader;
        if (!exheader_path.empty()) {
            exheader = ReadFile(exheader_path, 0x800);
            if (exheader.size() != 0x400 && exheader.size() != 0x800) {
                throw std::runtime_error("Decrypted ExHeader must be 0x400 or 0x800 bytes");
            }
        }
        image.MapImage(exheader, instruction_alignment);
        image.loaded_ = true;
        *this = std::move(image);
        return true;
    } catch (const std::exception& error) {
        std::cerr << "BinaryLoader: " << error.what() << ": " << filepath << '\n';
        return false;
    }
}

void BinaryLoader::MapImage(std::span<const std::uint8_t> exheader, std::uint32_t alignment) {
    if (alignment != 2 && alignment != 4) throw std::invalid_argument("Instruction alignment must be 2 or 4");
    if (code_.size() >= 4 &&
        ((code_[0] == 0x7F && code_[1] == 'E' && code_[2] == 'L' && code_[3] == 'F') ||
         (code_[0] == '3' && code_[1] == 'D' && code_[2] == 'S' && code_[3] == 'X'))) {
        throw std::runtime_error("ELF/3DSX containers must be converted to ExeFS .code first");
    }
    sections_[0].name = ".text";
    sections_[0].executable = true;
    sections_[1].name = ".rodata";
    sections_[2].name = ".data";
    sections_[2].writable = true;
    has_exheader_ = !exheader.empty();

    if (has_exheader_) {
        if ((exheader[0x0D] & 1u) != 0) {
            throw std::runtime_error("Compressed .code is unsupported; provide uncompressed metadata/payload");
        }
        // CodeSetInfo descriptors: virtual address, maximum pages, initialized size.
        constexpr std::array<std::size_t, 3> offsets{0x10, 0x20, 0x30};
        bss_size_ = ReadLE32(exheader, 0x3C);
        for (std::size_t i = 0; i < sections_.size(); ++i) {
            auto& section = sections_[i];
            section.virtual_address = ReadLE32(exheader, offsets[i]);
            const auto pages = ReadLE32(exheader, offsets[i] + 4);
            section.size = ReadLE32(exheader, offsets[i] + 8);
            const auto initialized_pages = PageSize(section.size) / kPageSize;
            if (pages < initialized_pages || pages > kFCRAMSize / kPageSize) {
                throw std::runtime_error("Invalid ExHeader section page count");
            }
            section.mapped_size = PageSize(std::uint64_t{section.size} +
                                           (i == 2 ? bss_size_ : 0u));
        }
        if (sections_[0].size == 0) throw std::runtime_error("ExHeader .text is empty");
    } else {
        sections_[0].virtual_address = kFCRAMBase;
        sections_[0].size = static_cast<std::uint32_t>(code_.size());
        sections_[0].mapped_size = PageSize(code_.size());
    }

    if (sections_[0].virtual_address % alignment != 0 || sections_[0].size % alignment != 0) {
        throw std::runtime_error("Text address/size disagrees with the requested instruction alignment");
    }
    entry_point_ = sections_[0].virtual_address; // NCCH starts at .text, not an ELF entry offset.

    std::uint64_t file_cursor = 0;
    std::uint64_t backing_cursor = 0;
    std::uint64_t required_length = 0;
    for (auto& section : sections_) {
        if (backing_cursor + section.mapped_size > kFCRAMSize || file_cursor > kFCRAMSize) {
            throw std::runtime_error("Image exceeds simulated FCRAM capacity");
        }
        section.file_offset = static_cast<std::uint32_t>(file_cursor);
        section.fcram_offset = static_cast<std::uint32_t>(backing_cursor);
        if (section.mapped_size != 0) {
            if (has_exheader_ && (section.virtual_address % kPageSize) != 0) {
                throw std::runtime_error("ExHeader section virtual address must be page aligned");
            }
            if (std::uint64_t{section.virtual_address} + section.mapped_size >
                (std::uint64_t{1} << 32)) {
                throw std::runtime_error("Section wraps the guest address space");
            }
        }
        if (section.size != 0) {
            required_length = std::max(required_length, file_cursor + section.size);
        }
        // File offsets follow page-rounded initialized sizes, never guest VA gaps.
        file_cursor += PageSize(section.size);
        backing_cursor += section.mapped_size;
    }
    if (code_.size() < required_length || code_.size() > file_cursor) {
        throw std::runtime_error("Uncompressed .code length disagrees with ExHeader sections");
    }
    for (std::size_t i = 0; i < sections_.size(); ++i) {
        const auto& a = sections_[i];
        if (a.mapped_size == 0) continue;
        for (std::size_t j = i + 1; j < sections_.size(); ++j) {
            const auto& b = sections_[j];
            if (b.mapped_size != 0 &&
                std::uint64_t{a.virtual_address} < std::uint64_t{b.virtual_address} + b.mapped_size &&
                std::uint64_t{b.virtual_address} < std::uint64_t{a.virtual_address} + a.mapped_size) {
                throw std::runtime_error("Guest section mappings overlap");
            }
        }
    }

    // BSS, unused page tails, and uninitialized backing bytes are always zero.
    mapped_memory_.resize(static_cast<std::size_t>(backing_cursor), 0);
    for (const auto& section : sections_) {
        if (section.size != 0) {
            std::copy_n(code_.begin() + section.file_offset, section.size,
                        mapped_memory_.begin() + section.fcram_offset);
        }
    }
    instruction_addresses_.reserve(sections_[0].size / alignment);
    for (std::uint32_t offset = 0; offset < sections_[0].size; offset += alignment) {
        instruction_addresses_.push_back(entry_point_ + offset);
    }
}

std::span<const std::uint8_t> BinaryLoader::GetTextBuffer() const noexcept {
    if (!loaded_) return {};
    return std::span<const std::uint8_t>{code_}.subspan(sections_[0].file_offset,
                                                      sections_[0].size);
}

std::span<const std::uint8_t>
BinaryLoader::GetGuestBytes(std::uint32_t address, std::size_t size) const {
    if (loaded_) {
        for (const auto& section : sections_) {
            if (section.mapped_size == 0 || address < section.virtual_address) continue;
            const auto offset = static_cast<std::size_t>(address - section.virtual_address);
            if (offset <= section.mapped_size && size <= section.mapped_size - offset) {
                return std::span<const std::uint8_t>{mapped_memory_}.subspan(
                    static_cast<std::size_t>(section.fcram_offset) + offset, size);
            }
        }
    }
    throw std::out_of_range("Guest byte range is not contained in a mapped section");
}

} // namespace arm11
