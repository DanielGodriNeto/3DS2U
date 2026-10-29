#include "ncsd_extractor.hpp"
#include "container_reader.hpp"
#include <array>

namespace extractor {
namespace {
using namespace detail;
Image ExtractCart(const Reader &read, std::uint64_t file_size, const KeyStore &keys,
                  ExtractionOptions options) {
    const auto header = read(0, 0x200);
    Need(std::equal(header.begin() + 0x100, header.begin() + 0x104, "NCSD"),
         "Expected an NCSD cartridge header");
    Need(Integer(header, 0x108, 8) != 0 && std::all_of(header.begin() + 0x110, header.begin() + 0x118,
                                                       [](auto value) { return value == 0; }),
         "Unsupported NCSD filesystem types or NAND image");
    const auto media_size = std::uint64_t{LE32(header, 0x104)} * 0x200u;
    Need(media_size >= 0x4200, "Invalid NCSD media size");
    struct Partition {
        std::uint64_t offset{}, size{};
    };
    std::array<Partition, 8> partitions{};
    for (unsigned i = 0; i < partitions.size(); ++i) {
        auto &partition = partitions[i];
        partition = {std::uint64_t{LE32(header, 0x120 + i * 8)} * 0x200u,
                     std::uint64_t{LE32(header, 0x124 + i * 8)} * 0x200u};
        if (partition.size == 0) {
            Need(partition.offset == 0 && i != 0, "Missing or malformed primary NCSD partition");
            continue;
        }
        Need(partition.offset >= 0x4000, "NCSD partition overlaps cartridge metadata");
        Range(media_size, partition.offset, partition.size);
        Range(file_size, partition.offset, partition.size);
        for (unsigned j = 0; j < i; ++j) {
            const auto &previous = partitions[j];
            if (previous.size)
                Need(partition.offset + partition.size <= previous.offset ||
                         previous.offset + previous.size <= partition.offset,
                     "Overlapping NCSD partitions");
        }
    }
    // The card's executable is partition 0; never silently substitute an update partition.
    const auto primary = partitions[0];
    const Reader partition_read = [&](std::uint64_t at, std::size_t size) {
        Range(primary.size, at, size);
        return read(primary.offset + at, size);
    };
    const auto ncch = partition_read(0, 0x200);
    const auto listed_id = Integer(header, 0x190, 8);
    Need(listed_id == 0 || listed_id == Integer(ncch, 0x108, 8),
         "NCSD partition ID table disagrees with NCCH");
    return ExtractNCCH(partition_read, primary.size, keys, options);
}
} // namespace
Image NCSDExtractor::Extract(std::span<const std::uint8_t> file, const KeyStore &keys,
                             ExtractionOptions options) const {
    detail::BinaryInput input(file);
    return ExtractCart([&](std::uint64_t at, std::size_t size) { return input.Read(at, size); },
                       input.GetSize(), keys, options);
}
Image NCSDExtractor::ExtractFile(const std::filesystem::path &filename, const KeyStore &keys,
                                 ExtractionOptions options) const {
    detail::BinaryInput input(filename);
    return ExtractCart([&](std::uint64_t at, std::size_t size) { return input.Read(at, size); },
                       input.GetSize(), keys, options);
}
} // namespace extractor
