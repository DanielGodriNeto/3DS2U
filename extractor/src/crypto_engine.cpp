#include "crypto_engine.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace extractor::crypto {
namespace {
constexpr std::uint8_t Multiply(std::uint8_t a, std::uint8_t b) {
    std::uint8_t result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (b & 1)
            result ^= a;
        a = static_cast<std::uint8_t>((a << 1) ^ ((a & 0x80) ? 0x11B : 0));
        b >>= 1;
    }
    return result;
}
constexpr auto SBox() {
    std::array<std::uint8_t, 256> table{};
    for (unsigned n = 0; n < 256; ++n) {
        std::uint8_t inverse = 1, base = static_cast<std::uint8_t>(n);
        unsigned exponent = 254;
        while (exponent) {
            if (exponent & 1)
                inverse = Multiply(inverse, base);
            base = Multiply(base, base);
            exponent >>= 1;
        }
        if (n == 0)
            inverse = 0;
        table[n] = static_cast<std::uint8_t>(inverse ^ std::rotl(inverse, 1) ^ std::rotl(inverse, 2) ^
                                             std::rotl(inverse, 3) ^ std::rotl(inverse, 4) ^ 0x63);
    }
    return table;
}
const auto sbox = SBox();
const auto inverse_sbox = [] {
    std::array<std::uint8_t, 256> table{};
    for (unsigned i = 0; i < 256; ++i)
        table[sbox[i]] = static_cast<std::uint8_t>(i);
    return table;
}();
const auto multiples = [] {
    std::array<std::array<std::uint8_t, 256>, 6> result{};
    constexpr std::array<std::uint8_t, 6> factors{2, 3, 9, 11, 13, 14};
    for (unsigned row = 0; row < factors.size(); ++row)
        for (unsigned value = 0; value < 256; ++value)
            result[row][value] = Multiply(static_cast<std::uint8_t>(value), factors[row]);
    return result;
}();
void AddRound(Block &block, std::span<const std::uint8_t> keys, unsigned round) {
    for (unsigned i = 0; i < 16; ++i)
        block[i] ^= keys[round * 16 + i];
}
void Shift(Block &block, bool inverse) {
    const auto previous = block;
    for (unsigned column = 0; column < 4; ++column)
        for (unsigned row = 0; row < 4; ++row)
            block[column * 4 + row] = previous[((column + (inverse ? 4 - row : row)) % 4) * 4 + row];
}
void Mix(Block &block, bool inverse) {
    const Block saved = block;
    for (unsigned col = 0; col < 4; ++col) {
        const auto start = col * 4;
        for (unsigned row = 0; row < 4; ++row) {
            const auto a = saved[start + row], b = saved[start + (row + 1) % 4],
                       c = saved[start + (row + 2) % 4], d = saved[start + (row + 3) % 4];
            block[start + row] = inverse
                                     ? static_cast<std::uint8_t>(multiples[5][a] ^ multiples[3][b] ^
                                                                 multiples[4][c] ^ multiples[2][d])
                                     : static_cast<std::uint8_t>(multiples[0][a] ^ multiples[1][b] ^ c ^ d);
        }
    }
}
void AddCounter(Block &counter, std::uint64_t amount) {
    unsigned carry = 0;
    for (unsigned i = 16; i != 0; --i) {
        const unsigned sum = counter[i - 1] + static_cast<unsigned>(amount & 255u) + carry;
        counter[i - 1] = static_cast<std::uint8_t>(sum);
        carry = sum >> 8;
        amount >>= 8;
    }
    if (carry || amount)
        throw std::overflow_error("AES counter overflow");
}
Block Rotate(Block block, unsigned count) {
    Block result{};
    for (unsigned bit = 0; bit < 128; ++bit) {
        const auto from = (bit + count) % 128;
        if ((block[from / 8] & (0x80u >> (from % 8))) != 0)
            result[bit / 8] |= static_cast<std::uint8_t>(0x80u >> (bit % 8));
    }
    return result;
}
} // namespace

AES128::AES128(const Block &key) {
    std::copy(key.begin(), key.end(), round_keys_.begin());
    std::uint8_t rcon = 1;
    for (unsigned at = 16; at < round_keys_.size(); at += 4) {
        std::array<std::uint8_t, 4> word{};
        std::copy_n(round_keys_.begin() + at - 4, 4, word.begin());
        if (at % 16 == 0) {
            word = {static_cast<std::uint8_t>(sbox[word[1]] ^ rcon), sbox[word[2]], sbox[word[3]],
                    sbox[word[0]]};
            rcon = Multiply(rcon, 2);
        }
        for (unsigned i = 0; i < 4; ++i)
            round_keys_[at + i] = round_keys_[at + i - 16] ^ word[i];
    }
}
Block AES128::Encrypt(Block block) const {
    AddRound(block, round_keys_, 0);
    for (unsigned round = 1; round <= 10; ++round) {
        for (auto &byte : block)
            byte = sbox[byte];
        Shift(block, false);
        if (round != 10)
            Mix(block, false);
        AddRound(block, round_keys_, round);
    }
    return block;
}
Block AES128::Decrypt(Block block) const {
    AddRound(block, round_keys_, 10);
    for (unsigned round = 10; round != 0; --round) {
        Shift(block, true);
        for (auto &byte : block)
            byte = inverse_sbox[byte];
        AddRound(block, round_keys_, round - 1);
        if (round != 1)
            Mix(block, true);
    }
    return block;
}
void AES128_CTR(std::span<std::uint8_t> bytes, const Block &key, Block counter, std::uint64_t offset) {
    if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - offset)
        throw std::overflow_error("AES stream range overflow");
    AddCounter(counter, offset / 16);
    const AES128 aes(key);
    std::size_t done = 0;
    auto skip = static_cast<std::size_t>(offset % 16);
    while (done < bytes.size()) {
        const auto stream = aes.Encrypt(counter);
        const auto length = std::min(bytes.size() - done, 16 - skip);
        for (std::size_t i = 0; i < length; ++i)
            bytes[done + i] ^= stream[skip + i];
        done += length;
        skip = 0;
        if (done != bytes.size())
            AddCounter(counter, 1);
    }
}
void AES128_CBCDecrypt(std::span<std::uint8_t> bytes, const Block &key, Block iv) {
    if (bytes.size() % 16 != 0)
        throw std::invalid_argument("CBC input is not block aligned");
    const AES128 aes(key);
    for (std::size_t at = 0; at < bytes.size(); at += 16) {
        Block cipher;
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(at), 16, cipher.begin());
        const auto plain = aes.Decrypt(cipher);
        for (unsigned i = 0; i < 16; ++i)
            bytes[at + i] = plain[i] ^ iv[i];
        iv = cipher;
    }
}
void AES128_CBCEncrypt(std::span<std::uint8_t> bytes, const Block &key, Block iv) {
    if (bytes.size() % 16 != 0)
        throw std::invalid_argument("CBC input is not block aligned");
    const AES128 aes(key);
    for (std::size_t at = 0; at < bytes.size(); at += 16) {
        for (unsigned i = 0; i < 16; ++i)
            iv[i] ^= bytes[at + i];
        iv = aes.Encrypt(iv);
        std::copy(iv.begin(), iv.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
    }
}
Block NormalKey(const Block &key_x, const Block &key_y) {
    // Nonsecret hardware scrambler constant, not an AES key.
    constexpr Block constant{0x1F, 0xF9, 0xE9, 0xAA, 0xC5, 0xFE, 0x04, 0x08,
                             0x02, 0x45, 0x91, 0xDC, 0x5D, 0x52, 0x76, 0x8A};
    auto value = Rotate(key_x, 2);
    unsigned carry = 0;
    for (unsigned i = 16; i != 0; --i) {
        const unsigned sum = (value[i - 1] ^ key_y[i - 1]) + constant[i - 1] + carry;
        value[i - 1] = static_cast<std::uint8_t>(sum);
        carry = sum >> 8;
    }
    return Rotate(value, 87);
}
Block NCCHFixedKey(std::uint64_t program_id, const Block* system_key) {
    if ((program_id & (std::uint64_t{0x10} << 32)) == 0) return {};
    if (!system_key) throw std::runtime_error("Fixed system NCCH requires keys.txt entry ncch_fixed_system_key");
    return *system_key;
}
Block NCCHCounter(std::span<const std::uint8_t, 8> id, std::uint16_t version, std::uint8_t section,
                  std::uint64_t section_offset) {
    if (section < 1 || section > 3 || version > 2)
        throw std::invalid_argument("Unsupported NCCH counter format");
    Block counter{};
    if (version == 1) {
        std::copy(id.begin(), id.end(), counter.begin());
        AddCounter(counter, section_offset);
    } else {
        std::reverse_copy(id.begin(), id.end(), counter.begin());
        counter[8] = section;
    }
    return counter;
}

void SHA256::Compress() {
    constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<std::uint32_t, 64> w{};
    for (unsigned i = 0; i < 16; ++i)
        for (unsigned j = 0; j < 4; ++j)
            w[i] = (w[i] << 8) | block_[i * 4 + j];
    for (unsigned i = 16; i < 64; ++i) {
        const auto a = w[i - 15], b = w[i - 2];
        w[i] = w[i - 16] + (std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3)) + w[i - 7] +
               (std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10));
    }
    auto s = state_;
    for (unsigned i = 0; i < 64; ++i) {
        const auto t1 = s[7] + (std::rotr(s[4], 6) ^ std::rotr(s[4], 11) ^ std::rotr(s[4], 25)) +
                        ((s[4] & s[5]) ^ (~s[4] & s[6])) + constants[i] + w[i];
        const auto t2 = (std::rotr(s[0], 2) ^ std::rotr(s[0], 13) ^ std::rotr(s[0], 22)) +
                        ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        s = {t1 + t2, s[0], s[1], s[2], s[3] + t1, s[4], s[5], s[6]};
    }
    for (unsigned i = 0; i < 8; ++i)
        state_[i] += s[i];
}
void SHA256::Update(std::span<const std::uint8_t> bytes) {
    if (finished_ || bytes.size() > std::numeric_limits<std::uint64_t>::max() / 8 - length_)
        throw std::invalid_argument("Invalid SHA256 stream state or length");
    length_ += bytes.size();
    for (const auto byte : bytes) {
        block_[used_++] = byte;
        if (used_ == 64) {
            Compress();
            used_ = 0;
        }
    }
}
Digest SHA256::Finish() {
    if (finished_)
        throw std::logic_error("SHA256 stream already finalized");
    block_[used_++] = 0x80;
    if (used_ > 56) {
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(used_), block_.end(), std::uint8_t{0});
        Compress();
        used_ = 0;
    }
    std::fill(block_.begin() + static_cast<std::ptrdiff_t>(used_), block_.begin() + 56, std::uint8_t{0});
    const auto bits = length_ * 8;
    for (unsigned i = 0; i < 8; ++i)
        block_[63 - i] = static_cast<std::uint8_t>(bits >> (i * 8));
    Compress();
    finished_ = true;
    Digest digest{};
    for (unsigned i = 0; i < 32; ++i)
        digest[i] = static_cast<std::uint8_t>(state_[i / 4] >> ((3 - i % 4) * 8));
    return digest;
}
Digest Hash(std::span<const std::uint8_t> bytes) {
    SHA256 hash;
    hash.Update(bytes);
    return hash.Finish();
}
} // namespace extractor::crypto
