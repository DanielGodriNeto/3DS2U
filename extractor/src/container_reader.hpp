#pragma once
#include "ncch_extractor.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace extractor::detail {
using Bytes = std::span<const std::uint8_t>;
using Buffer = std::vector<std::uint8_t>;
using Reader = ReadAt;
using crypto::Block;
inline void Need(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
inline void Range(std::uint64_t length, std::uint64_t at, std::uint64_t size) {
    Need(at <= length && size <= length - at, "Container range outside input");
}
inline std::uint64_t Integer(Bytes bytes, std::size_t at, unsigned count, bool big = false) {
    Need(count <= 8, "Invalid integer width");
    Range(bytes.size(), at, count);
    std::uint64_t result = 0;
    for (unsigned i = 0; i < count; ++i)
        result |= std::uint64_t{bytes[at + i]} << ((big ? count - 1 - i : i) * 8u);
    return result;
}
inline std::uint32_t LE32(Bytes bytes, std::size_t at) {
    return static_cast<std::uint32_t>(Integer(bytes, at, 4));
}
inline std::uint16_t BE16(Bytes bytes, std::size_t at) {
    return static_cast<std::uint16_t>(Integer(bytes, at, 2, true));
}
inline std::size_t Size(std::uint64_t size) {
    Need(size <= std::numeric_limits<std::size_t>::max() && size <= Buffer{}.max_size(),
         "Section exceeds host memory limits");
    return static_cast<std::size_t>(size);
}
inline std::string Hex(std::uint64_t value, unsigned width) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(width, '0');
    for (unsigned i = 0; i < width; ++i) {
        result[width - i - 1] = digits[value & 15u];
        value >>= 4;
    }
    return result;
}
inline void CheckDigest(Bytes actual, Bytes expected, const std::string &context, ExtractionOptions options) {
    if (actual.size() == expected.size() && std::equal(actual.begin(), actual.end(), expected.begin()))
        return;
    const auto digest_hex = [](Bytes bytes) {
        std::string result;
        for (auto byte : bytes)
            result += Hex(byte, 2);
        return result;
    };
    std::cerr << "[3DS2U Extractor] " << (options.ignore_hash ? "WARNING: " : "ERROR: ") << context
              << " hash mismatch\n"
              << "  Expected: " << digest_hex(expected) << "\n  Actual:   " << digest_hex(actual) << '\n';
    if (!options.ignore_hash)
        throw std::runtime_error(context + " hash mismatch");
    std::cerr << "[3DS2U Extractor] WARNING: Continuing because --ignore-hash is enabled.\n";
}
inline void CheckHash(Bytes bytes, Bytes expected, const std::string &context, ExtractionOptions options) {
    const auto hash = crypto::Hash(bytes);
    CheckDigest(hash, expected, context, options);
}
inline void CheckNCCHMagic(Bytes header, const std::string &context) {
    if (header.size() >= 0x104 && LE32(header, 0x100) == 0x4843434Eu)
        return;
    std::cerr << "[3DS2U Extractor] " << context << '\n'
              << "[3DS2U Extractor] FATAL: NCCH magic missing. Ticket TitleKey decryption failed "
                 "(check common_key) or KeyX is incorrect.\n"
              << "[3DS2U Extractor] NCCH magic is outside inner NCCH CTR encryption; "
                 "check outer CIA CBC, content selection or corruption first.\n";
    throw std::runtime_error("NCCH magic missing; cannot extract even with --ignore-hash");
}
// Synchronous bounded reads, shared by memory fixtures and multi-gigabyte disk images.
class BinaryInput {
  public:
    explicit BinaryInput(Bytes bytes) : bytes_(bytes), size_(bytes.size()) {}
    explicit BinaryInput(const std::filesystem::path &filename)
        : file_(filename, std::ios::binary | std::ios::ate) {
        Need(bool(file_), "Cannot open container input");
        const auto end = file_.tellg();
        Need(end >= 0, "Cannot size container input");
        size_ = static_cast<std::uint64_t>(end);
    }
    [[nodiscard]] std::uint64_t GetSize() const noexcept { return size_; }
    Buffer Read(std::uint64_t at, std::size_t size) {
        Range(size_, at, size);
        if (!file_.is_open()) {
            const auto start = bytes_.begin() + static_cast<std::ptrdiff_t>(at);
            return Buffer(start, start + static_cast<std::ptrdiff_t>(size));
        }
        Need(size <= static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()),
             "Container read exceeds stream size");
        file_.seekg(static_cast<std::streamoff>(at));
        Need(bool(file_), "Cannot seek container input");
        Buffer bytes(size);
        if (size)
            file_.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
        Need(bool(file_), "Truncated container read");
        return bytes;
    }

  private:
    Bytes bytes_;
    std::ifstream file_;
    std::uint64_t size_{};
};
} // namespace extractor::detail
