#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace extractor {
struct Image {
    std::vector<std::uint8_t> code, exheader, romfs;
    std::uint32_t relocations{}, text_size{}, rodata_size{}, data_size{}, bss_size{};
};
// Relocates an uncompressed, version-zero 3DSX to the runtime's fixed guest base.
[[nodiscard]] Image Extract(std::span<const std::uint8_t> file);
} // namespace extractor
