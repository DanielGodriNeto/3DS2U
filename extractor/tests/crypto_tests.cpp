#include "crypto_engine.hpp"
#include <algorithm>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace extractor::crypto;
void Check(bool value, std::source_location at = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("Crypto test failed at line " + std::to_string(at.line()));
}
std::vector<std::uint8_t> Hex(std::string_view text) {
    const auto nibble = [](char c) { return c >= 'a' ? c - 'a' + 10 : c - '0'; };
    Check(text.size() % 2 == 0);
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < text.size(); i += 2)
        bytes.push_back(static_cast<std::uint8_t>((nibble(text[i]) << 4) | nibble(text[i + 1])));
    return bytes;
}
Block Key(std::string_view text) {
    const auto bytes = Hex(text);
    Check(bytes.size() == 16);
    Block key;
    std::copy(bytes.begin(), bytes.end(), key.begin());
    return key;
}
template <class Operation> void Reject(Operation operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception &) {
        rejected = true;
    }
    Check(rejected);
}
} // namespace
int main() try {
    // Public NIST SP 800-38A test material, not Nintendo keys.
    const auto key = Key("2b7e151628aed2a6abf7158809cf4f3c");
    Check(NCCHFixedKey(0x0004000001234500ull) == Block{});
    Reject([&] { (void)NCCHFixedKey(0x0004001001234500ull); });
    Check(NCCHFixedKey(0x0004001001234500ull, &key) == key);
    const auto counter = Key("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    const auto plain = Hex("6bc1bee22e409f96e93d7e117393172a"
                           "ae2d8a571e03ac9c9eb76fac45af8e51"
                           "30c81c46a35ce411e5fbc1191a0a52ef"
                           "f69f2445df4f9b17ad2b417be66c3710");
    const auto cipher = Hex("874d6191b620e3261bef6864990db6ce"
                            "9806f66b7970fdff8617187bb9fffdff"
                            "5ae4df3edbd5d35e5b4f09020db03eab"
                            "1e031dda2fbe03d1792170a0f3009cee");
    auto bytes = cipher;
    AES128_CTR(bytes, key, counter);
    Check(bytes == plain);
    AES128_CTR(bytes, key, counter);
    Check(bytes == cipher);
    for (std::size_t offset = 0; offset < cipher.size(); ++offset) {
        const auto count = std::min<std::size_t>(19, cipher.size() - offset);
        bytes.assign(cipher.begin() + static_cast<std::ptrdiff_t>(offset),
                     cipher.begin() + static_cast<std::ptrdiff_t>(offset + count));
        AES128_CTR(bytes, key, counter, offset);
        Check(std::equal(bytes.begin(), bytes.end(), plain.begin() + static_cast<std::ptrdiff_t>(offset)));
    }
    const auto iv = Key("000102030405060708090a0b0c0d0e0f");
    Check(NormalKey(key, iv) == Key("03c44154c4e6f9a003365e2852d50d77"));
    const auto cbc = Hex("7649abac8119b246cee98e9b12e9197d"
                         "5086cb9b507219ee95db113a917678b2"
                         "73bed6b8e3c1743b7116e69e22229516"
                         "3ff1caa1681fac09120eca307586e1a7");
    bytes = cbc;
    AES128_CBCDecrypt(bytes, key, iv);
    Check(bytes == plain);
    AES128_CBCEncrypt(bytes, key, iv);
    Check(bytes == cbc);
    bytes.resize(17);
    Reject([&] { AES128_CBCDecrypt(bytes, key, iv); });
    const auto ecb = AES128(Key("000102030405060708090a0b0c0d0e0f"));
    Check(ecb.Encrypt(Key("00112233445566778899aabbccddeeff")) == Key("69c4e0d86a7b0430d8cdb78070b4c55a"));
    Block all_ones;
    all_ones.fill(255);
    bytes.assign(17, 0);
    Reject([&] { AES128_CTR(bytes, key, all_ones); });
    const std::array<std::uint8_t, 8> partition{0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    Check(NCCHCounter(partition, 2, 2) == Key("01020304050607080200000000000000"));
    Check(NCCHCounter(partition, 0, 3) == Key("01020304050607080300000000000000"));
    Check(NCCHCounter(partition, 1, 2, 0xA00) == Key("08070605040302010000000000000a00"));
    Reject([&] { (void)NCCHCounter(partition, 3, 2); });
    const auto hash = Hash(std::array<std::uint8_t, 3>{'a', 'b', 'c'});
    const auto expected = Hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Check(std::equal(hash.begin(), hash.end(), expected.begin()));
    SHA256 incremental;
    const std::array<std::uint8_t, 1> a{'a'};
    incremental.Update(a);
    incremental.Update(std::array<std::uint8_t, 2>{'b', 'c'});
    Check(incremental.Finish() == hash);
    Reject([&] { (void)incremental.Finish(); });
    const auto empty = Hash({});
    const auto empty_expected = Hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    Check(std::equal(empty.begin(), empty.end(), empty_expected.begin()));
    const std::string long_message = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const auto long_hash =
        Hash({reinterpret_cast<const std::uint8_t *>(long_message.data()), long_message.size()});
    const auto long_expected = Hex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Check(std::equal(long_hash.begin(), long_hash.end(), long_expected.begin()));
    std::cout << "NIST AES-128 CTR/CBC, ECB, stream offsets, SHA256 and NCCH counter tests passed.\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
