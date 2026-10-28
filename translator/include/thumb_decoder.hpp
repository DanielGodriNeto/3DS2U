#pragma once

#include "decoder.hpp"
#include <optional>
#include <string>

namespace arm11 {

class ThumbTranslator {
public:
    // Little-endian T16 only. Rejects incomplete halfwords and T32/long-BL prefixes.
    [[nodiscard]] std::vector<Instruction> DecodeBuffer(
        std::span<const std::uint8_t> buffer, std::uint32_t base_address = 0x08000000u) const;
    [[nodiscard]] std::string TranslateInstruction(std::uint16_t opcode,
                                                  std::uint32_t current_pc) const;
    [[nodiscard]] std::string TranslateInstruction(const Instruction& inst) const {
        return TranslateInstruction(static_cast<std::uint16_t>(inst.opcode), inst.address);
    }

    [[nodiscard]] static constexpr bool IsConditionalBranch(std::uint32_t op) noexcept {
        return (op & 0xF000u) == 0xD000u && ((op >> 8) & 15u) < 14u;
    }
    [[nodiscard]] static constexpr bool IsExchange(std::uint32_t op) noexcept {
        return (op & 0xFF07u) == 0x4700u;
    }
    [[nodiscard]] static constexpr bool IsPopPC(std::uint32_t op) noexcept {
        return (op & 0xFF00u) == 0xBD00u;
    }
    [[nodiscard]] static constexpr bool IsTerminator(std::uint32_t op) noexcept {
        return IsConditionalBranch(op) || (op & 0xF800u) == 0xE000u ||
               IsExchange(op) || IsPopPC(op);
    }
    [[nodiscard]] static constexpr std::optional<std::uint32_t>
    BranchTarget(std::uint32_t op, std::uint32_t pc) noexcept {
        if (IsConditionalBranch(op)) {
            auto offset = op & 0xFFu;
            if ((offset & 0x80u) != 0) offset |= 0xFFFFFF00u;
            return pc + 4u + (offset << 1);
        }
        if ((op & 0xF800u) == 0xE000u) {
            auto offset = op & 0x7FFu;
            if ((offset & 0x400u) != 0) offset |= 0xFFFFF800u;
            return pc + 4u + (offset << 1);
        }
        return std::nullopt;
    }
};

} // namespace arm11
