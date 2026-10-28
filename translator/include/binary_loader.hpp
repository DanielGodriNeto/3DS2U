#pragma once

#include <cstdint>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace arm11 {

struct BinarySection {
    std::string_view name;
    std::uint32_t virtual_address{};
    std::uint32_t file_offset{};
    std::uint32_t size{};          // Initialized bytes, excluding file/page padding.
    std::uint32_t mapped_size{};   // Page allocation; .data includes zeroed BSS.
    std::uint32_t fcram_offset{};  // Backing offset from BinaryLoader::kFCRAMBase.
    bool executable{};
    bool writable{};
};

// ExeFS .code has no embedded section table or entry-point field. Real-image
// layouts come from a decrypted NCCH ExHeader; no-header mode is raw ARM.
class BinaryLoader {
public:
    static constexpr std::uint32_t kFCRAMBase = 0x08000000u;
    static constexpr std::uint32_t kFCRAMSize = 128u * 1024u * 1024u;
    static constexpr std::uint32_t kPageSize = 0x1000u;

    // Accepts uncompressed .code plus a 0x400/0x800-byte decrypted ExHeader.
    // Without metadata the whole input is ARM .text. Failure clears all views.
    [[nodiscard]] bool LoadFile(const std::string& filepath,
                                const std::string& exheader_path = {});

    // Borrowed view: invalidated by LoadFile(), destruction, or owner assignment.
    [[nodiscard]] std::span<const std::uint8_t> GetCodeBuffer() const noexcept {
        return code_;
    }

    [[nodiscard]] std::span<const std::uint8_t> GetTextBuffer() const noexcept;
    [[nodiscard]] std::span<const BinarySection> GetSections() const noexcept {
        return loaded_ ? std::span<const BinarySection>{sections_} :
                         std::span<const BinarySection>{};
    }
    [[nodiscard]] std::uint32_t GetEntryPoint() const noexcept { return entry_point_; }
    [[nodiscard]] std::uint32_t GetBssSize() const noexcept { return bss_size_; }
    [[nodiscard]] bool HasExHeader() const noexcept { return has_exheader_; }

    // Backing starts at kFCRAMBase; section descriptors retain original guest VAs.
    // Only populated pages are allocated, with a 128 MiB total capacity limit.
    [[nodiscard]] std::span<const std::uint8_t> GetMappedMemory() const noexcept {
        return mapped_memory_;
    }
    [[nodiscard]] std::span<const std::uint8_t>
    GetGuestBytes(std::uint32_t address, std::size_t size) const;

    // Aligned ARM candidates from initialized .text only. ExHeader metadata
    // cannot distinguish Thumb instructions or literal pools inside .text.
    [[nodiscard]] std::span<const std::uint32_t> GetInstructionAddresses() const noexcept {
        return instruction_addresses_;
    }

private:
    void MapImage(std::span<const std::uint8_t> exheader);
    std::vector<std::uint8_t> code_;
    std::array<BinarySection, 3> sections_{};
    std::vector<std::uint8_t> mapped_memory_;
    std::vector<std::uint32_t> instruction_addresses_;
    std::uint32_t entry_point_{};
    std::uint32_t bss_size_{};
    bool has_exheader_{};
    bool loaded_{};
};

} // namespace arm11
