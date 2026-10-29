#pragma once

#include <cstdint>
#include <string>

namespace arm11 {

class ConditionEvaluator {
public:
    // AL returns an empty expression; unsupported cond=0xF is guarded by false.
    // ARMv6 unconditional-extension encodings need a separate decoding path.
    // Throws std::invalid_argument when cond_code exceeds four bits.
    [[nodiscard]] static std::string GetConditionString(std::uint8_t cond_code);
};

} // namespace arm11
