#pragma once
#include "crypto_engine.hpp"
#include <filesystem>
#include <map>
#include <string>

namespace extractor {
class KeyStore {
  public:
    void LoadFile(const std::filesystem::path &filename);
    void Set(const std::string &name, const crypto::Block &key);
    [[nodiscard]] const crypto::Block *Find(const std::string &name) const;
    [[nodiscard]] crypto::Block Require(const std::string &name) const;
    [[nodiscard]] crypto::Block CommonKey(std::uint8_t index) const;

  private:
    std::map<std::string, crypto::Block> keys_;
};
} // namespace extractor
