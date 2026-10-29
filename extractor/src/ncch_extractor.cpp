#include "ncch_extractor.hpp"
#include "container_reader.hpp"
namespace extractor {
using namespace detail;
Image ExtractNCCH(const ReadAt &read, std::uint64_t content_size, const KeyStore &keys) {
    const auto fetch = [&](std::uint64_t at, std::size_t length) {
        Range(content_size, at, length);
        auto bytes = read(at, length);
        Need(bytes.size() == length, "Short NCCH read");
        return bytes;
    };
    const auto header = fetch(0, 0x200);
    Need(std::equal(header.begin() + 0x100, header.begin() + 0x104, "NCCH"),
         "Selected partition is not an NCCH executable");
    const auto length = std::uint64_t{LE32(header, 0x104)} * 0x200u;
    Range(content_size, 0, length);
    Need(length >= 0xA00, "NCCH is too small for an executable");
    Need(LE32(header, 0x180) == 0x400, "NCCH has no supported executable ExHeader");
    const auto version = static_cast<std::uint16_t>(Integer(header, 0x112, 2));
    const auto program = Integer(header, 0x118, 8);
    const std::span<const std::uint8_t, 8> partition{header.data() + 0x108, 8};
    const bool encrypted = (header[0x18F] & 4u) == 0;
    Block primary{}, secondary{};
    if (encrypted) {
        if ((header[0x18F] & 1u) != 0) {
            const auto *system_key = keys.Find("ncch_fixed_system_key");
            if (!system_key)
                system_key = keys.Find("ncch_fixed_key");
            primary = secondary = crypto::NCCHFixedKey(program, system_key);
        } else {
            Block y{};
            std::copy_n(header.begin(), 16, y.begin());
            primary = crypto::NormalKey(keys.Require("keyx_2c"), y);
            std::uint8_t slot{};
            switch (header[0x18B]) {
            case 0:
                slot = 0x2C;
                break;
            case 1:
                slot = 0x25;
                break;
            case 0xA:
                slot = 0x18;
                break;
            case 0xB:
                slot = 0x1B;
                break;
            default:
                throw std::runtime_error("Unsupported NCCH crypto method");
            }
            if ((header[0x18F] & 0x20u) != 0) {
                const auto seed = keys.Require("seed_" + Hex(program, 16));
                std::array<std::uint8_t, 24> check{};
                std::copy(seed.begin(), seed.end(), check.begin());
                std::copy_n(header.begin() + 0x118, 8, check.begin() + 16);
                const auto digest = crypto::Hash(check);
                Need(std::equal(digest.begin(), digest.begin() + 4, header.begin() + 0x114),
                     "NCCH seed verification failed");
                std::array<std::uint8_t, 32> seed_y{};
                std::copy(y.begin(), y.end(), seed_y.begin());
                std::copy(seed.begin(), seed.end(), seed_y.begin() + 16);
                const auto derived = crypto::Hash(seed_y);
                std::copy_n(derived.begin(), 16, y.begin());
            }
            secondary = crypto::NormalKey(keys.Require("keyx_" + Hex(slot, 2)), y);
        }
    }
    const auto read_region = [&](std::uint64_t base, std::uint64_t at, std::size_t size, std::uint8_t section,
                                 const Block &key) {
        Range(length, base, at);
        Range(length - base, at, size);
        auto bytes = fetch(base + at, size);
        if (encrypted)
            crypto::AES128_CTR(bytes, key, crypto::NCCHCounter(partition, version, section, base), at);
        return bytes;
    };
    const auto exefs_at = std::uint64_t{LE32(header, 0x1A0)} * 0x200u;
    const auto exefs_size = std::uint64_t{LE32(header, 0x1A4)} * 0x200u;
    const auto romfs_at = std::uint64_t{LE32(header, 0x1B0)} * 0x200u;
    const auto romfs_size = std::uint64_t{LE32(header, 0x1B4)} * 0x200u;
    Need(exefs_at >= 0xA00 && exefs_size >= 0x200, "Invalid ExeFS range");
    Range(length, exefs_at, exefs_size);
    if (romfs_size) {
        Need(romfs_at >= exefs_at + exefs_size, "NCCH regions overlap");
        Range(length, romfs_at, romfs_size);
    }
    Image image;
    image.exheader = read_region(0x200, 0, 0x400, 1, primary);
    CheckHash(image.exheader, Bytes(header).subspan(0x160, 32));
    const auto exefs = read_region(exefs_at, 0, 0x200, 2, primary);
    Need(LE32(header, 0x1A8) == 1, "Unsupported ExeFS superblock size");
    CheckHash(exefs, Bytes(header).subspan(0x1C0, 32));
    bool found = false;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> used;
    for (unsigned i = 0; i < 10; ++i) {
        const auto record = i * 16u;
        if (exefs[record] == 0)
            continue;
        const auto at = std::uint64_t{LE32(exefs, record + 8)} + 0x200u;
        const auto size = LE32(exefs, record + 12);
        Range(exefs_size, at, size);
        for (const auto &[start, count] : used)
            Need(at + size <= start || start + count <= at, "Overlapping ExeFS files");
        used.emplace_back(at, size);
        constexpr std::array<std::uint8_t, 8> code_name{'.', 'c', 'o', 'd', 'e', 0, 0, 0};
        if (std::equal(code_name.begin(), code_name.end(), exefs.begin() + record)) {
            Need(!found && size <= 256u * 1024u * 1024u, "Invalid or duplicate .code entry");
            found = true;
            image.code = read_region(exefs_at, at, size, 2, secondary);
            CheckHash(image.code, Bytes(exefs).subspan(0xC0 + (9u - i) * 32u, 32));
        }
    }
    Need(found, "NCCH ExeFS has no .code file");
    if ((image.exheader[0x0D] & 1u) != 0) {
        image.code = DecompressCodeLZSS(image.code);
        image.exheader[0x0D] &= 0xFEu;
    }
    image.text_size = LE32(image.exheader, 0x18);
    image.rodata_size = LE32(image.exheader, 0x28);
    image.data_size = LE32(image.exheader, 0x38);
    image.bss_size = LE32(image.exheader, 0x3C);
    Need(image.text_size != 0, "Executable .text is empty");
    std::uint64_t cursor = 0, minimum = 0;
    for (const auto size : {image.text_size, image.rodata_size, image.data_size}) {
        if (size)
            minimum = cursor + size;
        cursor += (std::uint64_t{size} + 0xFFFu) & ~std::uint64_t{0xFFF};
    }
    Need(image.code.size() >= minimum && image.code.size() <= cursor,
         ".code length disagrees with ExHeader segments");
    if (romfs_size) {
        const auto ivfc = read_region(romfs_at, 0, 0x60, 3, secondary);
        Need(std::equal(ivfc.begin(), ivfc.begin() + 4, "IVFC") && LE32(ivfc, 4) == 0x10000,
             "Unsupported RomFS IVFC header");
        const auto log = LE32(ivfc, 0x4C);
        Need(log >= 9 && log <= 24, "Invalid RomFS Level-3 block alignment");
        const auto alignment = std::uint64_t{1} << log;
        const auto lv3 = (0x60u + std::uint64_t{LE32(ivfc, 8)} + alignment - 1) & ~(alignment - 1);
        const auto size = Integer(ivfc, 0x44, 8);
        Range(romfs_size, lv3, size);
        Need(size >= 0x28, "RomFS Level-3 data is empty");
        image.romfs = read_region(romfs_at, lv3, Size(size), 3, secondary);
    }
    return image;
}

std::vector<std::uint8_t> DecompressCodeLZSS(Bytes code) {
    Need(code.size() >= 8, "Truncated .code BLZ footer");
    const auto footer = LE32(code, code.size() - 8), extra = LE32(code, code.size() - 4);
    const auto compressed = footer & 0xFFFFFFu, header = footer >> 24;
    Need(header >= 8 && header <= compressed && compressed <= code.size(), "Invalid .code BLZ footer");
    const auto end = std::uint64_t{code.size()} + extra;
    Need(end <= 256u * 1024u * 1024u, "Decompressed .code exceeds 256 MiB");
    Buffer result(code.begin(), code.end());
    result.resize(Size(end));
    const auto bottom = code.size() - compressed;
    auto input = code.size() - header, output = result.size();
    while (output > bottom) {
        Need(input > bottom, "Truncated .code BLZ flags");
        const auto flags = code[--input];
        for (unsigned bit = 0; bit < 8 && output > bottom; ++bit) {
            if ((flags & (0x80u >> bit)) != 0) {
                Need(input - bottom >= 2, "Truncated .code BLZ backreference");
                const auto high = code[--input], low = code[--input];
                const unsigned token = (unsigned(high) << 8) | low;
                const auto count = (token >> 12) + 3, distance = (token & 0xFFFu) + 2;
                Need(count <= output - bottom, "BLZ output underrun");
                for (unsigned i = 0; i < count; ++i) {
                    --output;
                    Need(distance < result.size() - output, "BLZ backreference outside decoded output");
                    result[output] = result[output + distance];
                }
            } else {
                Need(input > bottom, "Truncated .code BLZ literal");
                result[--output] = code[--input];
            }
        }
    }
    Need(input == bottom, "BLZ stream has unused compressed bytes");
    return result;
}
} // namespace extractor
