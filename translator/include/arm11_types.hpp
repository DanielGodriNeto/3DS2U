#pragma once

#include "cfg_builder.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace arm11 {

// Architectural register values are host-native integers, never byte-swapped.
// This initial context models unbanked registers and the CPSR flag subset only.
struct CPUContext {
    struct CPSR {
        bool N{};
        bool Z{};
        bool C{};
        bool V{};
    };

    std::uint32_t r[16]{}; // R13 = SP, R14 = LR, R15 = PC.
    CPSR cpsr{};
    bool thumb{}; // Execution-state bit, separate from the NZCV flag subset.

    constexpr void BranchExchange(std::uint32_t target) noexcept {
        thumb = (target & 1u) != 0;
        r[15] = target & (thumb ? ~1u : ~3u);
    }

    // Architectural masks retain the existing flag-helper interface.
    static constexpr std::uint32_t N = std::uint32_t{1} << 31;
    static constexpr std::uint32_t Z = std::uint32_t{1} << 30;
    static constexpr std::uint32_t C = std::uint32_t{1} << 29;
    static constexpr std::uint32_t V = std::uint32_t{1} << 28;

    constexpr void SetFlag(std::uint32_t mask, bool value) noexcept {
        if ((mask & N) != 0) cpsr.N = value;
        if ((mask & Z) != 0) cpsr.Z = value;
        if ((mask & C) != 0) cpsr.C = value;
        if ((mask & V) != 0) cpsr.V = value;
    }

    [[nodiscard]] constexpr bool Flag(std::uint32_t mask) const noexcept {
        return (((mask & N) != 0) && cpsr.N) ||
               (((mask & Z) != 0) && cpsr.Z) ||
               (((mask & C) != 0) && cpsr.C) ||
               (((mask & V) != 0) && cpsr.V);
    }

    // Logical instructions update N/Z while preserving C/V unless specified.
    constexpr void UpdateNZ(std::uint32_t result) noexcept {
        SetFlag(N, (result & N) != 0);
        SetFlag(Z, result == 0);
    }

    // The decoder supplies carry-out (or NOT-borrow for subtraction).
    constexpr void UpdateC(bool carry) noexcept { SetFlag(C, carry); }
    constexpr void UpdateV(bool overflow) noexcept { SetFlag(V, overflow); }

    constexpr void UpdateNZCV(std::uint32_t result, bool carry,
                              bool overflow) noexcept {
        UpdateNZ(result);
        UpdateC(carry);
        UpdateV(overflow);
    }
};

// One contiguous guest-address window backed by zero-initialized FCRAM.
// Defaults to the original 3DS physical FCRAM window. Virtual mappings, MMIO,
// permissions, and instruction-specific ARM alignment behavior belong above it.
class Memory {
public:
    static constexpr std::uint32_t kFCRAMBase = 0x20000000u;
    static constexpr std::size_t kFCRAMSize = 128u * 1024u * 1024u;

    explicit Memory(std::size_t size = kFCRAMSize,
                    std::uint32_t guest_base = kFCRAMBase)
        : base_(guest_base), bytes_(CheckedSize(size, guest_base)) {}

    // memcpy avoids host alignment and strict-aliasing undefined behavior.
    [[nodiscard]] inline std::uint32_t Read32(std::uint32_t address) const {
        std::uint32_t value;
        std::memcpy(&value, bytes_.data() + Offset(address, sizeof(value)),
                    sizeof(value));
        return LittleEndian(value);
    }

    inline void Write32(std::uint32_t address, std::uint32_t value) {
        const auto offset = Offset(address, sizeof(value));
        value = LittleEndian(value);
        std::memcpy(bytes_.data() + offset, &value, sizeof(value));
    }

    [[nodiscard]] inline std::uint16_t Read16(std::uint32_t address) const {
        std::uint16_t value;
        std::memcpy(&value, bytes_.data() + Offset(address, sizeof(value)),
                    sizeof(value));
        return LittleEndian(value);
    }

    inline void Write16(std::uint32_t address, std::uint16_t value) {
        const auto offset = Offset(address, sizeof(value));
        value = LittleEndian(value);
        std::memcpy(bytes_.data() + offset, &value, sizeof(value));
    }

    // Loader access: bytes always retain the guest's little-endian layout.
    [[nodiscard]] inline std::span<std::byte> Bytes() noexcept { return bytes_; }
    [[nodiscard]] inline std::span<const std::byte> Bytes() const noexcept {
        return bytes_;
    }

private:
    // Reject address windows that would wrap the 32-bit guest address space.
    static inline std::size_t CheckedSize(std::size_t size, std::uint32_t base) {
        constexpr auto limit = std::uint64_t{1} << 32;
        if (size == 0 || size > limit - base) {
            throw std::invalid_argument("Invalid FCRAM address window");
        }
        return size;
    }

    // Subtraction-based bounds checks avoid address + width overflow.
    [[nodiscard]] inline std::size_t Offset(std::uint32_t address,
                                           std::size_t width) const {
        if (address < base_) {
            throw std::out_of_range("FCRAM address below mapped window");
        }
        const auto offset = static_cast<std::size_t>(address - base_);
        if (offset > bytes_.size() || width > bytes_.size() - offset) {
            throw std::out_of_range("FCRAM access outside mapped window");
        }
        return offset;
    }

    // Symmetric conversion: guest LE <-> host native. Wii U always swaps;
    // little-endian PC tools do not. C++20 has no std::byteswap.
    template <typename T>
    [[nodiscard]] static inline T LittleEndian(T value) noexcept {
        static_assert(sizeof(T) == 2 || sizeof(T) == 4);
        static_assert(std::numeric_limits<unsigned char>::digits == 8);
        static_assert(std::endian::native == std::endian::little ||
                      std::endian::native == std::endian::big);
        if constexpr (std::endian::native == std::endian::big) {
#if defined(__GNUC__) || defined(__clang__)
            if constexpr (sizeof(T) == 4) {
                return static_cast<T>(__builtin_bswap32(value));
            } else {
                return static_cast<T>(__builtin_bswap16(value));
            }
#else
            // Portable fallback for non-GCC PC toolchains.
            if constexpr (sizeof(T) == 4) {
                return static_cast<T>(((value & 0x000000FFu) << 24) |
                                      ((value & 0x0000FF00u) << 8) |
                                      ((value & 0x00FF0000u) >> 8) |
                                      ((value & 0xFF000000u) >> 24));
            } else {
                return static_cast<T>((value << 8) | (value >> 8));
            }
#endif
        }
        return value;
    }

    std::uint32_t base_;
    std::vector<std::byte> bytes_;
};

// The loader supplies raw .code at its configured virtual address, or each
// executable ELF load region at its ELF virtual address; never decode ELF headers.
// Non-owning input: backing binary storage must outlive the Disassembler.
struct ExecutableRegion {
    std::uint32_t virtual_address{};
    std::span<const std::byte> bytes;
};

// PC-side analysis interface only; opcode decoding is implemented separately.
class Disassembler {
public:
    // Copies region descriptors; their underlying binary buffers remain borrowed.
    // Implementation validates non-overlapping regions and 32-bit address ranges.
    explicit Disassembler(std::span<const ExecutableRegion> executable);

    // Future implementation validates alignment, mapped bytes, and decode limits;
    // errors are reported with exceptions, never silent partial block results.
    [[nodiscard]] BasicBlock DecodeBlock(CodeLocation start) const;

    // Reachability traversal follows direct edges; indirect edges stay marked.
    [[nodiscard]] std::vector<BasicBlock>
    Analyze(std::span<const CodeLocation> entry_points) const;

private:
    std::vector<ExecutableRegion> regions_;
};

} // namespace arm11
