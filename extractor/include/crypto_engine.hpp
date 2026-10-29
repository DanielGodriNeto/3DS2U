#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace extractor::crypto {
using Block = std::array<std::uint8_t, 16>;
using Digest = std::array<std::uint8_t, 32>;
class AES128 {
  public:
    explicit AES128(const Block &key);
    [[nodiscard]] Block Encrypt(Block block) const;
    [[nodiscard]] Block Decrypt(Block block) const;

  private:
    std::array<std::uint8_t, 176> round_keys_{};
};
// Counter bytes are big-endian; byte_offset supports seeking inside an NCCH region.
void AES128_CTR(std::span<std::uint8_t> data, const Block &key, Block counter, std::uint64_t byte_offset = 0);
void AES128_CBCDecrypt(std::span<std::uint8_t> data, const Block &key, Block iv);
void AES128_CBCEncrypt(std::span<std::uint8_t> data, const Block &key, Block iv);
[[nodiscard]] Block NormalKey(const Block &key_x, const Block &key_y);
// Fixed application NCCH uses zero; fixed system NCCH requires a supplied key.
[[nodiscard]] Block NCCHFixedKey(std::uint64_t program_id, const Block* system_key = nullptr);
[[nodiscard]] Block NCCHCounter(std::span<const std::uint8_t, 8> partition_id, std::uint16_t version,
                                std::uint8_t section, std::uint64_t section_offset = 0);
class SHA256 {
  public:
    void Update(std::span<const std::uint8_t> bytes);
    [[nodiscard]] Digest Finish();

  private:
    void Compress();
    std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> block_{};
    std::uint64_t length_{};
    std::size_t used_{};
    bool finished_{};
};
[[nodiscard]] Digest Hash(std::span<const std::uint8_t> bytes);
} // namespace extractor::crypto
