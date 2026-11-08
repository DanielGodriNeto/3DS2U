#pragma once

#include "cfg_builder.hpp"
#include "decoder.hpp"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace arm11 {

// A 4-byte literal-pool word (aligned down) that holds data, not code.
struct DataRegion {
    std::uint32_t address{};
    std::uint32_t size{};
};

// Forced-Thumb ranges are (start, byte size) constraints; they do not seed discovery.
struct DiscoveryOptions {
    bool all_thumb{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> thumb_ranges{};
};

struct DiscoveryResult {
    std::vector<Instruction> instructions; // Address-ascending, non-overlapping.
    std::vector<DataRegion> literal_pools; // Address-ascending, disjoint from code.
};

// Follows control flow from entry only. Rejects ambiguous input with std::invalid_argument
// whose message contains the guest address; branch targets outside text are ignored.
[[nodiscard]] DiscoveryResult DiscoverCode(std::span<const std::uint8_t> text,
                                           std::uint32_t base_address, CodeLocation entry,
                                           const DiscoveryOptions& options = {});

} // namespace arm11
