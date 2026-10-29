#pragma once
#include "ncch_extractor.hpp"
namespace extractor {
class CIAExtractor {
  public:
    [[nodiscard]] Image Extract(std::span<const std::uint8_t> file, const KeyStore &keys) const;
    [[nodiscard]] Image ExtractFile(const std::filesystem::path &filename, const KeyStore &keys) const;
};
} // namespace extractor
