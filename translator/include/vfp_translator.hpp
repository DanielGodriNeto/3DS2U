#pragma once
#include "decoder.hpp"
#include <string>

namespace arm11 {
class VFPTranslator {
public:
    [[nodiscard]] static constexpr bool IsEncoding(std::uint32_t op) noexcept {
        return (op & 0x0C000E00u) == 0x0C000A00u;
    }
    [[nodiscard]] std::string TranslateInstruction(const Instruction& instruction) const;
};
} // namespace arm11
