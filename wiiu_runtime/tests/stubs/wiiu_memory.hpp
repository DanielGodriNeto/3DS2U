#pragma once
#include "pica200.hpp"
#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace wiiu {
// Host test memory; production FCRAM allocation/BE swapping is built with devkitPPC.
class WiiUMemory {
public:
    WiiUMemory() : bytes_(0x10000) {}
    explicit WiiUMemory(Pica200& bridge) : WiiUMemory() { SetPica200(bridge); }
    void SetPica200(Pica200& bridge) { pica_ = &bridge; bridge.AttachMemory(*this); }
    void RequestFrame() noexcept { frame_requested_ = true; }
    void ReserveImageRange(std::uint32_t address, std::size_t size) { (void)Offset(address, size); }
    bool ConsumeFrameRequest() noexcept {
        const bool result = frame_requested_;
        frame_requested_ = false;
        return result;
    }
    const void* GetPointer(std::uint32_t addr, std::size_t size = 1) const {
        return bytes_.data() + Offset(addr, size);
    }
    void LoadImage(std::uint32_t addr, std::span<const std::uint8_t> source) {
        std::copy(source.begin(), source.end(), bytes_.begin() + Offset(addr, source.size()));
    }
    void ClearRange(std::uint32_t addr, std::size_t size) {
        const auto offset = Offset(addr, size);
        std::fill_n(bytes_.begin() + offset, size, std::uint8_t{0});
    }
    std::uint32_t Read32(std::uint32_t addr) const {
        const auto* p = static_cast<const std::uint8_t*>(GetPointer(addr, 4));
        return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) |
               (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
    }
    std::uint8_t Read8(std::uint32_t addr) const { return bytes_[Offset(addr, 1)]; }
    void Write8(std::uint32_t addr, std::uint8_t value) { bytes_[Offset(addr, 1)] = value; }
    void Write32(std::uint32_t addr, std::uint32_t value) {
        last_address = addr; last_value = value; ++writes;
        if (addr >= Pica200::kMMIOBase && addr < Pica200::kMMIOEnd) {
            if (pica_ == nullptr) throw std::logic_error("Missing test bridge");
            pica_->WriteMMIO(addr, value);
            return;
        }
        const auto offset = Offset(addr, 4);
        for (unsigned i = 0; i < 4; ++i) bytes_[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
    std::uint32_t last_address{}, last_value{};
    unsigned writes{};
private:
    std::size_t Offset(std::uint32_t addr, std::size_t size) const {
        if (addr < 0x08000000u) throw std::out_of_range("Test guest address below image");
        const auto offset = static_cast<std::size_t>(addr - 0x08000000u);
        if (offset > bytes_.size() || size > bytes_.size() - offset) throw std::out_of_range("Test image bounds");
        return offset;
    }
    std::vector<std::uint8_t> bytes_;
    Pica200* pica_{};
    bool frame_requested_{};
};
} // namespace wiiu
