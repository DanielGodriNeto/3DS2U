#include "cia_extractor.hpp"
#include "container_reader.hpp"
#include <bit>
#include <optional>
#include <set>
namespace extractor {
namespace {
using namespace detail;
std::uint64_t Align64(std::uint64_t value) {
    Need(value <= std::numeric_limits<std::uint64_t>::max() - 63, "CIA offset overflow");
    return (value + 63) & ~std::uint64_t{63};
}
std::size_t SignedBody(Bytes bytes) {
    switch (Integer(bytes, 0, 4, true)) {
    case 0x10003:
        return 0x240;
    case 0x10004:
        return 0x140;
    case 0x10005:
        return 0x80;
    default:
        throw std::runtime_error("Unsupported CIA signature format; expected SHA256 ticket/TMD");
    }
}
struct Content {
    Reader source;
    std::uint64_t offset{}, size{};
    Block key{}, iv{};
    bool encrypted{};
    Buffer Read(std::uint64_t at, std::size_t length) const {
        Range(size, at, length);
        if (!encrypted)
            return source(offset + at, length);
        const auto start = at & ~std::uint64_t{15};
        const auto skip = static_cast<std::size_t>(at - start);
        const auto padded = (std::uint64_t{length} + skip + 15u) & ~std::uint64_t{15};
        Range(size, start, padded);
        auto previous = iv;
        if (start != 0) {
            const auto bytes = source(offset + start - 16, 16);
            std::copy(bytes.begin(), bytes.end(), previous.begin());
        }
        auto bytes = source(offset + start, Size(padded));
        crypto::AES128_CBCDecrypt(bytes, key, previous);
        return {bytes.begin() + static_cast<std::ptrdiff_t>(skip),
                bytes.begin() + static_cast<std::ptrdiff_t>(skip + length)};
    }
};

Image ExtractCIA(const Reader &read, std::uint64_t file_size, const KeyStore &keys,
                 ExtractionOptions options) {
    const auto header = read(0, 0x2020);
    Need(LE32(header, 0) == 0x2020 && Integer(header, 6, 2) == 0, "Unsupported CIA header");
    const auto cert_size = LE32(header, 8), ticket_size = LE32(header, 0xC), tmd_size = LE32(header, 0x10);
    const auto content_size = Integer(header, 0x18, 8);
    const auto cert_at = Align64(header.size()), ticket_at = Align64(cert_at + cert_size),
               tmd_at = Align64(ticket_at + ticket_size), content_at = Align64(tmd_at + tmd_size);
    Range(file_size, cert_at, cert_size);
    Range(file_size, ticket_at, ticket_size);
    Range(file_size, tmd_at, tmd_size);
    Range(file_size, content_at, content_size);
    if (LE32(header, 0x14))
        Range(file_size, content_at + Align64(content_size), LE32(header, 0x14));
    Need(ticket_size <= 1024u * 1024u && tmd_size <= 4u * 1024u * 1024u, "CIA metadata is too large");
    const auto ticket = read(ticket_at, ticket_size), tmd = read(tmd_at, tmd_size);
    const auto tk = SignedBody(ticket), tm = SignedBody(tmd);
    Range(ticket.size(), tk, 0x164);
    Range(tmd.size(), tm, 0x9C4);
    Need(Integer(ticket, tk + 0x98, 4, true) == 0,
         "Personalized CIA tickets require device-specific ticket conversion");
    Need(Integer(ticket, tk + 0x9C, 8, true) == Integer(tmd, tm + 0x4C, 8, true),
         "Ticket and TMD title IDs disagree");
    const auto key_index = ticket[tk + 0xB1];
    const auto context = "Title ID 0x" + Hex(Integer(ticket, tk + 0x9C, 8, true), 16) + ", CommonKey index " +
                         std::to_string(key_index);
    std::cerr << "[3DS2U Extractor] CIA " << context << '\n';
    const auto count = BE16(tmd, tm + 0x9E), boot = BE16(tmd, tm + 0xA0);
    Need(count != 0, "CIA contains no content records");
    Range(tmd.size(), tm + 0x9C4, std::uint64_t{count} * 48);
    CheckHash(Bytes(tmd).subspan(tm + 0xC4, 0x900), Bytes(tmd).subspan(tm + 0xA4, 32),
              "TMD content-info table (unencrypted metadata; " + context + ")", options);
    std::vector<bool> covered(count, false);
    for (unsigned i = 0; i < 64; ++i) {
        const auto at = tm + 0xC4 + i * 0x24;
        const auto start = BE16(tmd, at), number = BE16(tmd, at + 2);
        if (!number)
            continue;
        Range(count, start, number);
        for (unsigned n = start; n < unsigned(start) + number; ++n) {
            Need(!covered[n], "Overlapping TMD content-info records");
            covered[n] = true;
        }
        CheckHash(Bytes(tmd).subspan(tm + 0x9C4 + start * 48u, number * 48u), Bytes(tmd).subspan(at + 4, 32),
                  "TMD chunk records, group " + std::to_string(i) + " (unencrypted metadata; " + context +
                      ")",
                  options);
    }
    Need(std::all_of(covered.begin(), covered.end(), [](bool value) { return value; }),
         "Incomplete TMD content-info table");
    Block title_key{};
    bool have_title = false;
    std::uint64_t cursor = 0;
    std::set<std::uint16_t> indices;
    std::set<std::uint32_t> ids;
    std::optional<Content> selected;
    crypto::Digest expected{};
    unsigned included = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto record = tm + 0x9C4 + i * 48u;
        const auto index = BE16(tmd, record + 4), type = BE16(tmd, record + 6);
        Need(indices.insert(index).second &&
                 ids.insert(static_cast<std::uint32_t>(Integer(tmd, record, 4, true))).second,
             "Duplicate CIA content ID/index");
        if ((header[0x20 + index / 8] & (0x80u >> (index % 8))) == 0)
            continue;
        ++included;
        const auto size = Integer(tmd, record + 8, 8, true);
        Range(content_size, cursor, size);
        Need(size != 0, "Empty CIA content");
        Content content{read, content_at + cursor, size, {}, {}, (type & 1u) != 0};
        // Outer CIA encryption is independent of the inner NCCH crypto flags.
        if (size >= 0x104) {
            const auto prefix = read(content.offset, 0x104);
            Need(prefix.size() == 0x104, "Short CIA content header read");
            if (LE32(prefix, 0x100) == 0x4843434Eu) {
                std::cerr << "[3DS2U Extractor] CIA content is already decrypted (NoCrypto). "
                             "Skipping outer AES-CBC pass.\n";
                content.encrypted = false;
            }
        }
        if (content.encrypted) {
            Need(size % 16 == 0, "Encrypted CIA content is not block aligned");
            if (!have_title) {
                const auto suffix = std::to_string(key_index);
                const auto key_name = keys.Find("common_key_" + suffix) ? "common_key_" + suffix
                                      : key_index == 0 && keys.Find("common_key")
                                          ? std::string{"common_key"}
                                          : "keyx_3d + common_key_y_" + suffix;
                std::cerr << "[3DS2U Extractor] Ticket TitleKey AES-CBC: requesting " << key_name << " ("
                          << context << ")\n";
                std::copy_n(ticket.begin() + static_cast<std::ptrdiff_t>(tk + 0x7F), 16, title_key.begin());
                Block iv{};
                std::copy_n(ticket.begin() + static_cast<std::ptrdiff_t>(tk + 0x9C), 8, iv.begin());
                crypto::AES128_CBCDecrypt(title_key, keys.CommonKey(key_index), iv);
                have_title = true;
            }
            content.key = title_key;
            content.iv[0] = static_cast<std::uint8_t>(index >> 8);
            content.iv[1] = static_cast<std::uint8_t>(index);
        }
        if (index == boot) {
            selected = content;
            std::copy_n(tmd.begin() + static_cast<std::ptrdiff_t>(record + 16), 32, expected.begin());
        }
        cursor += size;
    }
    unsigned bitmap_count = 0;
    for (std::size_t i = 0x20; i < header.size(); ++i)
        bitmap_count += static_cast<unsigned>(std::popcount(header[i]));
    Need(cursor == content_size && bitmap_count == included && selected.has_value(),
         "CIA content bitmap/length or boot index is invalid");
    const auto content_context =
        "CIA TMD content chunk, index " + std::to_string(boot) +
        (selected->encrypted ? " (after outer AES-CBC; " : " (unencrypted content; ") + context + ")";
    std::cerr << "[3DS2U Extractor] " << content_context << "; checking NCCH magic before SHA256\n";
    CheckNCCHMagic(selected->Read(0, 0x200), content_context);
    crypto::SHA256 hash;
    for (std::uint64_t at = 0; at < selected->size;) {
        const auto length =
            static_cast<std::size_t>(std::min<std::uint64_t>(1024u * 1024u, selected->size - at));
        hash.Update(selected->Read(at, length));
        at += length;
    }
    const auto actual = hash.Finish();
    CheckDigest(actual, expected, content_context, options);
    return ExtractNCCH([&](std::uint64_t at, std::size_t length) { return selected->Read(at, length); },
                       selected->size, keys, options);
}
} // namespace

Image CIAExtractor::Extract(Bytes file, const KeyStore &keys, ExtractionOptions options) const {
    BinaryInput input(file);
    return ExtractCIA([&](std::uint64_t at, std::size_t size) { return input.Read(at, size); },
                      input.GetSize(), keys, options);
}
Image CIAExtractor::ExtractFile(const std::filesystem::path &filename, const KeyStore &keys,
                                ExtractionOptions options) const {
    BinaryInput input(filename);
    return ExtractCIA([&](std::uint64_t at, std::size_t size) { return input.Read(at, size); },
                      input.GetSize(), keys, options);
}
} // namespace extractor
