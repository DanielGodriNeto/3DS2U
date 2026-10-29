#pragma once

#include "decoder.hpp"
#include <optional>
#include <string>

namespace arm11 {

class ThumbTranslator {
public:
    // LE mixed-width Thumb fetch; a wide opcode stores h1 in its upper halfword.
    [[nodiscard]] std::vector<Instruction> DecodeBuffer(
        std::span<const std::uint8_t> buffer, std::uint32_t base_address = 0x08000000u) const;
    [[nodiscard]] std::string TranslateInstruction(std::uint32_t opcode,
                                                  std::uint32_t current_pc) const;
    [[nodiscard]] std::string TranslateInstruction(const Instruction& inst) const;
    [[nodiscard]] static constexpr bool IsWidePrefix(std::uint32_t halfword) noexcept {
        return (halfword & 0xF800u) >= 0xE800u;
    }
    [[nodiscard]] static constexpr bool IsWideBranch(std::uint32_t op) noexcept {
        const auto kind = op & 0xD000u;
        return op > 0xFFFFu && ((op >> 16) & 0xF800u) == 0xF000u &&
               (kind == 0xD000u || kind == 0xC000u || kind == 0x9000u);
    }
    [[nodiscard]] static constexpr bool IsBLXImmediate(std::uint32_t op) noexcept {
        return IsWideBranch(op) && (op & 0xD000u) == 0xC000u;
    }
    [[nodiscard]] static constexpr bool IsWideLoadPC(std::uint32_t op) noexcept {
        const auto head = (op >> 16) & 0xFFF0u;
        return op > 0xFFFFu && (head == 0xF8D0u || head == 0xF850u) && (op & 0xF000u) == 0xF000u;
    }

    [[nodiscard]] static constexpr bool IsConditionalBranch(std::uint32_t op) noexcept {
        return op <= 0xFFFFu && (op & 0xF000u) == 0xD000u && ((op >> 8) & 15u) < 14u;
    }
    [[nodiscard]] static constexpr bool IsExchange(std::uint32_t op) noexcept {
        return op <= 0xFFFFu && (op & 0xFF07u) == 0x4700u;
    }
    [[nodiscard]] static constexpr bool IsPopPC(std::uint32_t op) noexcept {
        return op <= 0xFFFFu && (op & 0xFF00u) == 0xBD00u;
    }
    [[nodiscard]] static constexpr bool IsTerminator(std::uint32_t op) noexcept {
        if (op > 0xFFFFu) return IsWideBranch(op) || IsWideLoadPC(op);
        return IsConditionalBranch(op) || (op & 0xF800u) == 0xE000u ||
               IsExchange(op) || IsPopPC(op) || (op & 0xFF00u) == 0xDF00u;
    }
    [[nodiscard]] static constexpr std::optional<std::uint32_t>
    BranchTarget(std::uint32_t op, std::uint32_t pc) noexcept {
        if (op > 0xFFFFu) {
            if (!IsWideBranch(op) || (IsBLXImmediate(op) && (op & 1u) != 0)) return std::nullopt;
            const auto first = op >> 16, sign = (first >> 10) & 1u;
            const auto i1 = 1u ^ (((op >> 13) & 1u) ^ sign);
            const auto i2 = 1u ^ (((op >> 11) & 1u) ^ sign);
            auto offset = (sign << 24) | (i1 << 23) | (i2 << 22) | ((first & 0x3FFu) << 12) | ((op & 0x7FFu) << 1);
            if (sign != 0) offset |= 0xFE000000u;
            return (IsBLXImmediate(op) ? (pc + 4u) & ~3u : pc + 4u) + offset;
        }
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
