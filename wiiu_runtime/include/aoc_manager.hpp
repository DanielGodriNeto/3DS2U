#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace wiiu {
class WiiUMemory;

// Project DLC shim: these command headers are not the native 3DS AM protocol.
class AocManager {
  public:
    static constexpr std::uint32_t kInvalidArgument = 0xD9040001u;
    static constexpr std::uint32_t kNotSupported = 0xD9040002u;
    static constexpr std::uint32_t kCountCommand = 0x00020000u;
    static constexpr std::uint32_t kListCommand = 0x00030042u;

    // The caller supplies the installed DLC manifest for this game, not other titles.
    void SetInstalledDLCs(std::uint64_t game_title_id, std::span<const std::uint64_t> title_ids);
    [[nodiscard]] std::uint64_t GetGameTitleID() const noexcept { return game_title_id_; }
    [[nodiscard]] std::span<const std::uint64_t> GetInstalledTitleIDs() const noexcept { return titles_; }
    [[nodiscard]] std::uint32_t CountAddOnContent() const noexcept;
    [[nodiscard]] std::uint32_t HandleIPC(WiiUMemory &memory, std::uint32_t command_address) const;

  private:
    std::uint64_t game_title_id_{};
    std::vector<std::uint64_t> titles_;
};
} // namespace wiiu
