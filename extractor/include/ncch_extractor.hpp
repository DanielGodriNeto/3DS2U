#pragma once
#include "key_store.hpp"
#include "three_dsx.hpp"
#include <functional>

namespace extractor {
using ReadAt = std::function<std::vector<std::uint8_t>(std::uint64_t, std::size_t)>;
// Reader offsets start at the NCCH header, after any outer-container decryption.
[[nodiscard]] Image ExtractNCCH(const ReadAt &read, std::uint64_t content_size, const KeyStore &keys);
[[nodiscard]] std::vector<std::uint8_t> DecompressCodeLZSS(std::span<const std::uint8_t> code);
} // namespace extractor
