#pragma once
#include "ncch_extractor.hpp"

namespace extractor {
class NCSDExtractor {
  public:
    [[nodiscard]] Image Extract(std::span<const std::uint8_t> file, const KeyStore &keys,
                                ExtractionOptions options = {}) const;
    [[nodiscard]] Image ExtractFile(const std::filesystem::path &filename, const KeyStore &keys,
                                    ExtractionOptions options = {}) const;
};
} // namespace extractor
