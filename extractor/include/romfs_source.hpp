#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>

namespace extractor {

// Streams the C++ source that embeds a RomFS image behind wiiu::GetPackagedRomFS().
// Bytes are written as adjacent "\xNN" string literals of 64 bytes per line. GCC
// parses these far faster and with far less memory than a 0xNN, initializer list.
// The terminating NUL of the literal is excluded from the returned span.
inline void WriteRomFSSource(std::ostream& out, std::span<const std::uint8_t> bytes) {
    out << "// Generated RomFS package; bytes remain in guest little-endian order.\n"
           "#include \"packaged_romfs.hpp\"\nnamespace wiiu {\n";
    if (bytes.empty()) {
        out << "std::span<const std::uint8_t> GetPackagedRomFS() noexcept { return {}; }\n"
               "} // namespace wiiu\n";
        return;
    }
    constexpr char hex[] = "0123456789ABCDEF";
    out << "namespace { alignas(32) const std::uint8_t image[] =\n\"";
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out << "\\x" << hex[bytes[i] >> 4] << hex[bytes[i] & 15];
        if ((i & 63u) == 63u && i + 1 != bytes.size()) out << "\"\n\"";
    }
    out << "\"; }\nstd::span<const std::uint8_t> GetPackagedRomFS() noexcept "
           "{ return std::span<const std::uint8_t>{image, sizeof(image) - 1}; }\n"
           "} // namespace wiiu\n";
}

} // namespace extractor
