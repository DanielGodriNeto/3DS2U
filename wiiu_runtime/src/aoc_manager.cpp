#include "aoc_manager.hpp"
#include "wiiu_memory.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace wiiu {
namespace {
void Reply(WiiUMemory &memory, std::uint32_t address, std::span<const std::uint32_t> words) {
    (void)memory.GetPointer(address, words.size_bytes());
    for (std::size_t i = 0; i < words.size(); ++i)
        memory.Write32(address + static_cast<std::uint32_t>(i * 4u), words[i]);
}
} // namespace

void AocManager::SetInstalledDLCs(std::uint64_t game, std::span<const std::uint64_t> titles) {
    if (game == 0 || titles.size() > 4096 || std::find(titles.begin(), titles.end(), 0) != titles.end())
        throw std::invalid_argument("Invalid game or DLC title manifest");
    std::vector<std::uint64_t> installed(titles.begin(), titles.end());
    std::sort(installed.begin(), installed.end());
    installed.erase(std::unique(installed.begin(), installed.end()), installed.end());
    titles_ = std::move(installed);
    game_title_id_ = game;
}

std::uint32_t AocManager::CountAddOnContent() const noexcept {
    return static_cast<std::uint32_t>(titles_.size());
}

std::uint32_t AocManager::HandleIPC(WiiUMemory &memory, std::uint32_t address) const {
    std::uint32_t command{};
    try {
        if ((address & 3u) != 0)
            return kInvalidArgument;
        (void)memory.GetPointer(address, 4);
        command = memory.Read32(address);
        if (command == kCountCommand) {
            Reply(memory, address, std::array{0x00020080u, 0u, CountAddOnContent()});
            return 0;
        }
        if (command != kListCommand) {
            Reply(memory, address, std::array{(command & 0xFFFF0000u) | 0x40u, kNotSupported});
            return kNotSupported;
        }
        (void)memory.GetPointer(address, 20);
        const auto capacity = memory.Read32(address + 4);
        const auto descriptor = memory.Read32(address + 8);
        const auto output = memory.Read32(address + 12);
        // Each 64-bit Title ID is two LE32 words, never a truncated title ID.
        const auto bytes = std::uint64_t{capacity} * 8u;
        if (bytes > 0x0FFFFFFFu || descriptor != ((static_cast<std::uint32_t>(bytes) << 4) | 0xCu))
            throw std::invalid_argument("Invalid AOC output buffer descriptor");
        if (bytes != 0) {
            if ((output & 3u) != 0)
                throw std::invalid_argument("Unaligned AOC output buffer");
            (void)memory.GetPointer(output, static_cast<std::size_t>(bytes));
            if (std::uint64_t{output} < std::uint64_t{address} + 20u &&
                std::uint64_t{address} < std::uint64_t{output} + bytes)
                throw std::invalid_argument("AOC output overlaps IPC command buffer");
        }
        const auto count = std::min(capacity, CountAddOnContent());
        for (std::uint32_t i = 0; i < count; ++i) {
            memory.Write32(output + i * 8u, static_cast<std::uint32_t>(titles_[i]));
            memory.Write32(output + i * 8u + 4u, static_cast<std::uint32_t>(titles_[i] >> 32));
        }
        Reply(memory, address, std::array{0x00030082u, 0u, count, descriptor, output});
        return 0;
    } catch (const std::exception &) {
        try {
            Reply(memory, address, std::array{(command & 0xFFFF0000u) | 0x40u, kInvalidArgument});
        } catch (const std::exception &) {
        }
        return kInvalidArgument;
    }
}
} // namespace wiiu
