#pragma once
#include "key_store.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <vector>
namespace extractor::test {

using namespace extractor;
using crypto::Block;
using Buffer = std::vector<std::uint8_t>;
inline void Check(bool value, std::source_location at = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("CIA test failed at line " + std::to_string(at.line()));
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
inline void Put(Buffer &bytes, std::size_t at, std::uint64_t value, unsigned width = 4, bool big = false) {
    for (unsigned i = 0; i < width; ++i)
        bytes.at(at + i) = static_cast<std::uint8_t>(value >> ((big ? width - 1 - i : i) * 8));
}
inline void HashAt(Buffer &bytes, std::size_t at, std::span<const std::uint8_t> source) {
    const auto hash = crypto::Hash(source);
    std::copy(hash.begin(), hash.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
}
inline std::size_t Align(std::size_t value, std::size_t alignment = 64) {
    return (value + alignment - 1) & ~(alignment - 1);
}
inline const Block common{0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                          0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
inline const Block title_key{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
constexpr std::uint64_t title_id = 0x0004000001234500ull;
constexpr std::uint16_t content_index = 0x123;
inline Buffer RomFS() {
    Buffer bytes(0x90);
    const std::array<std::uint32_t, 10> header{0x28, 0x28, 4, 0x2C, 24, 0x44, 4, 0x48, 48, 0x78};
    for (unsigned i = 0; i < 10; ++i)
        Put(bytes, i * 4, header[i]);
    Put(bytes, 0x30, 0xFFFFFFFF);
    Put(bytes, 0x34, 0xFFFFFFFF);
    Put(bytes, 0x3C, 0xFFFFFFFF);
    Put(bytes, 0x4C, 0xFFFFFFFF);
    Put(bytes, 0x58, 14, 8);
    Put(bytes, 0x60, 0xFFFFFFFF);
    Put(bytes, 0x64, 16);
    constexpr char name[] = "test.txt";
    for (unsigned i = 0; i < 8; ++i)
        bytes[0x68 + i * 2] = name[i];
    constexpr char text[] = "3DS2U_ROMFS_OK";
    std::copy_n(text, 14, bytes.begin() + 0x78);
    return bytes;
}
inline Buffer Compressed() {
    Buffer bytes{0xFE, 0xFF, 0xFF, 0xEA, 0x00, 0x10, 0x00, 0xF0, 'A', 'A', 0x30};
    bytes.resize(19);
    Put(bytes, 11, 0x0800000F);
    Put(bytes, 15, 9);
    return bytes;
}
struct Fixture {
    Buffer cia, ncch, code, romfs;
    KeyStore keys;
    std::size_t content_at{};
};
inline Fixture Make(bool outer = true, bool inner = true, bool compressed = false, bool seed_crypto = false,
                    std::uint8_t method = 0, std::uint16_t version = 2, bool fixed = false,
                    bool system = false) {
    Fixture result;
    result.romfs = RomFS();
    result.code = compressed ? Buffer(28, 'A') : Buffer{0xFE, 0xFF, 0xFF, 0xEA};
    if (compressed)
        std::copy_n(std::array<std::uint8_t, 4>{0xFE, 0xFF, 0xFF, 0xEA}.begin(), 4, result.code.begin());
    const auto stored = compressed ? Compressed() : result.code;
    result.keys.Set("common_key_0", common);
    result.keys.Set("keyx_2c", common);
    result.keys.Set("keyx_25", title_key);
    result.keys.Set("keyx_18", title_key);
    result.keys.Set("keyx_1b", title_key);
    const std::size_t exefs_at = 0xA00, exefs_size = Align(0x200 + stored.size(), 0x200);
    const auto romfs_at = exefs_at + exefs_size, romfs_size = Align(0x1000 + result.romfs.size(), 0x200);
    Buffer ncch(romfs_at + romfs_size);
    std::copy(common.begin(), common.end(), ncch.begin());
    std::copy_n("NCCH", 4, ncch.begin() + 0x100);
    Put(ncch, 0x104, ncch.size() / 0x200);
    Put(ncch, 0x108, 0x0102030405060708ull, 8);
    Put(ncch, 0x112, version, 2);
    Put(ncch, 0x118, title_id | (system ? (std::uint64_t{0x10} << 32) : 0), 8);
    Put(ncch, 0x180, 0x400);
    ncch[0x18B] = method;
    ncch[0x18F] = static_cast<std::uint8_t>((inner ? 0 : 4) | (seed_crypto ? 0x20 : 0) | (fixed ? 1 : 0));
    Put(ncch, 0x1A0, exefs_at / 0x200);
    Put(ncch, 0x1A4, exefs_size / 0x200);
    Put(ncch, 0x1A8, 1);
    Put(ncch, 0x1B0, romfs_at / 0x200);
    Put(ncch, 0x1B4, romfs_size / 0x200);
    auto secondary_y = common;
    if (seed_crypto) {
        result.keys.Set("seed_0004000001234500", title_key);
        std::array<std::uint8_t, 24> seed_check{};
        std::copy(title_key.begin(), title_key.end(), seed_check.begin());
        std::copy_n(ncch.begin() + 0x118, 8, seed_check.begin() + 16);
        const auto seed_hash = crypto::Hash(seed_check);
        std::copy_n(seed_hash.begin(), 4, ncch.begin() + 0x114);
        std::array<std::uint8_t, 32> input{};
        std::copy(common.begin(), common.end(), input.begin());
        std::copy(title_key.begin(), title_key.end(), input.begin() + 16);
        const auto hash = crypto::Hash(input);
        std::copy_n(hash.begin(), 16, secondary_y.begin());
    }
    ncch[0x20D] = compressed ? 1 : 0;
    Put(ncch, 0x210, 0x08000000);
    Put(ncch, 0x214, 1);
    Put(ncch, 0x218, result.code.size());
    Put(ncch, 0x220, 0x08001000);
    Put(ncch, 0x230, 0x08001000);
    HashAt(ncch, 0x160, std::span{ncch}.subspan(0x200, 0x400));
    std::copy_n(".code", 5, ncch.begin() + static_cast<std::ptrdiff_t>(exefs_at));
    Put(ncch, exefs_at + 12, stored.size());
    HashAt(ncch, exefs_at + 0x1E0, stored);
    std::copy(stored.begin(), stored.end(), ncch.begin() + static_cast<std::ptrdiff_t>(exefs_at + 0x200));
    HashAt(ncch, 0x1C0, std::span{ncch}.subspan(exefs_at, 0x200));
    std::copy_n("IVFC", 4, ncch.begin() + static_cast<std::ptrdiff_t>(romfs_at));
    Put(ncch, romfs_at + 4, 0x10000);
    Put(ncch, romfs_at + 8, 32);
    Put(ncch, romfs_at + 0x44, result.romfs.size(), 8);
    Put(ncch, romfs_at + 0x4C, 12);
    std::copy(result.romfs.begin(), result.romfs.end(),
              ncch.begin() + static_cast<std::ptrdiff_t>(romfs_at + 0x1000));
    if (inner) {
        const auto primary = fixed ? (system ? title_key : Block{}) : crypto::NormalKey(common, common);
        const auto secondary =
            fixed ? primary : crypto::NormalKey(method == 0 ? common : title_key, secondary_y);
        if (fixed && system)
            result.keys.Set("ncch_fixed_system_key", title_key);
        const std::span<const std::uint8_t, 8> id{ncch.data() + 0x108, 8};
        crypto::AES128_CTR(std::span{ncch}.subspan(0x200, 0x800), primary,
                           crypto::NCCHCounter(id, version, 1, 0x200));
        crypto::AES128_CTR(std::span{ncch}.subspan(exefs_at, 0x200), primary,
                           crypto::NCCHCounter(id, version, 2, exefs_at));
        crypto::AES128_CTR(std::span{ncch}.subspan(exefs_at + 0x200, exefs_size - 0x200), secondary,
                           crypto::NCCHCounter(id, version, 2, exefs_at), 0x200);
        crypto::AES128_CTR(std::span{ncch}.subspan(romfs_at, romfs_size), secondary,
                           crypto::NCCHCounter(id, version, 3, romfs_at));
    }
    result.ncch = ncch;
    Buffer ticket(0x350), tmd(0xB34);
    Put(ticket, 0, 0x10004, 4, true);
    Put(tmd, 0, 0x10004, 4, true);
    Put(ticket, 0x140 + 0x9C, title_id, 8, true);
    auto encrypted_title = title_key;
    Block iv{};
    std::copy_n(ticket.begin() + 0x1DC, 8, iv.begin());
    crypto::AES128_CBCEncrypt(encrypted_title, common, iv);
    std::copy(encrypted_title.begin(), encrypted_title.end(), ticket.begin() + 0x1BF);
    Put(tmd, 0x18C, title_id, 8, true);
    Put(tmd, 0x1DE, 1, 2, true);
    Put(tmd, 0x1E0, content_index, 2, true);
    Put(tmd, 0xB04, 0x12345678, 4, true);
    Put(tmd, 0xB08, content_index, 2, true);
    Put(tmd, 0xB0A, outer ? 1 : 0, 2, true);
    Put(tmd, 0xB0C, ncch.size(), 8, true);
    HashAt(tmd, 0xB14, ncch);
    Put(tmd, 0x206, 1, 2, true);
    HashAt(tmd, 0x208, std::span{tmd}.subspan(0xB04, 48));
    HashAt(tmd, 0x1E4, std::span{tmd}.subspan(0x204, 0x900));
    if (outer) {
        Block content_iv{};
        content_iv[0] = static_cast<std::uint8_t>(content_index >> 8);
        content_iv[1] = static_cast<std::uint8_t>(content_index);
        crypto::AES128_CBCEncrypt(ncch, title_key, content_iv);
    }
    const auto ticket_at = Align(Align(0x2020) + 16), tmd_at = Align(ticket_at + ticket.size());
    result.content_at = Align(tmd_at + tmd.size());
    result.cia.resize(result.content_at + ncch.size());
    Put(result.cia, 0, 0x2020);
    Put(result.cia, 8, 16);
    Put(result.cia, 12, ticket.size());
    Put(result.cia, 0x10, tmd.size());
    Put(result.cia, 0x18, ncch.size(), 8);
    result.cia[0x20 + content_index / 8] = static_cast<std::uint8_t>(0x80u >> (content_index % 8));
    std::copy(ticket.begin(), ticket.end(), result.cia.begin() + static_cast<std::ptrdiff_t>(ticket_at));
    std::copy(tmd.begin(), tmd.end(), result.cia.begin() + static_cast<std::ptrdiff_t>(tmd_at));
    std::copy(ncch.begin(), ncch.end(), result.cia.begin() + static_cast<std::ptrdiff_t>(result.content_at));
    return result;
}
inline void Write(const std::filesystem::path &path, std::span<const std::uint8_t> bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    Check(!file.fail());
}

} // namespace extractor::test
