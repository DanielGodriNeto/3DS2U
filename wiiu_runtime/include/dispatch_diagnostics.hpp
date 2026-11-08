#pragma once

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace wiiu {

// Why a dispatch lookup found no block. Pure: no coreinit, memory or engine dependencies.
enum class MissingBlockKind {
    MisalignedTarget,  // Invalid control flow: ARM PC not 4-byte or Thumb PC not 2-byte aligned.
    UnmappedTarget,    // Guest memory at the PC is not backed; the target is not code.
    NoTranslatedBlock, // PC is mapped memory with no registered block (not checked for executability).
    CroModuleMiss,     // PC is inside a loaded CRO module (code or data; executable flag not checked).
};

// Rules apply in order: alignment, then mapping, then module ownership.
[[nodiscard]] inline MissingBlockKind ClassifyMissingBlock(std::uint32_t pc, bool thumb,
                                                           bool mapped,
                                                           bool inside_cro_module) noexcept {
    const std::uint32_t alignment_mask = thumb ? 1u : 3u;
    if ((pc & alignment_mask) != 0) return MissingBlockKind::MisalignedTarget;
    if (!mapped) return MissingBlockKind::UnmappedTarget;
    if (inside_cro_module) return MissingBlockKind::CroModuleMiss;
    return MissingBlockKind::NoTranslatedBlock;
}

[[nodiscard]] inline const char* MissingBlockKindName(MissingBlockKind kind) noexcept {
    switch (kind) {
    case MissingBlockKind::MisalignedTarget: return "MisalignedTarget";
    case MissingBlockKind::UnmappedTarget: return "UnmappedTarget";
    case MissingBlockKind::NoTranslatedBlock: return "NoTranslatedBlock";
    case MissingBlockKind::CroModuleMiss: return "CroModuleMiss";
    }
    return "Unknown";
}

// Longest module name written to a detail line; longer names are cut.
inline constexpr std::size_t kMaxModuleNameLength = 31;

// Copies at most kMaxModuleNameLength bytes, replacing non-printable bytes with '?'.
// A null or empty name becomes "-". The destination is always NUL-terminated.
inline void CopyModuleNameForDetail(char (&destination)[kMaxModuleNameLength + 1],
                                    const char* module_name) noexcept {
    if (module_name == nullptr || *module_name == '\0') {
        destination[0] = '-';
        destination[1] = '\0';
        return;
    }
    std::size_t length = 0;
    for (; length < kMaxModuleNameLength && module_name[length] != '\0'; ++length) {
        const auto byte = static_cast<unsigned char>(module_name[length]);
        destination[length] = (byte < 0x20u || byte >= 0x7Fu) ? '?' : module_name[length];
    }
    destination[length] = '\0';
}

// Writes one NUL-terminated detail line into out (no host paths, no memory contents).
// Returns the characters written, excluding the NUL; output is cut to capacity - 1.
// A zero capacity writes nothing.
inline std::size_t FormatMissingBlockDetail(char* out, std::size_t capacity, MissingBlockKind kind,
                                            std::uint32_t pc, bool thumb,
                                            std::uint32_t caller_key, bool has_caller,
                                            const char* module_name) noexcept {
    if (out == nullptr || capacity == 0) return 0;
    char module[kMaxModuleNameLength + 1]{};
    CopyModuleNameForDetail(module, module_name);
    char caller[16]{"-"};
    if (has_caller) std::snprintf(caller, sizeof caller, "0x%08" PRIX32, caller_key);
    const int written = std::snprintf(
        out, capacity, "[3DS2U] Missing block detail: kind=%s pc=0x%08" PRIX32 " mode=%s caller=%s module=%s",
        MissingBlockKindName(kind), pc, thumb ? "Thumb" : "ARM", caller, module);
    if (written < 0) {
        out[0] = '\0';
        return 0;
    }
    const auto length = static_cast<std::size_t>(written);
    return length < capacity ? length : capacity - 1;
}

} // namespace wiiu
