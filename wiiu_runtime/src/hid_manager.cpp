#include "hid_manager.hpp"

#include <vpad/input.h>

#include <algorithm>
#include <stdexcept>

namespace wiiu {
namespace {
constexpr std::uint32_t MapButtons(std::uint32_t buttons) noexcept {
    constexpr std::uint32_t mapping[] = {
        VPAD_BUTTON_A, VPAD_BUTTON_B, VPAD_BUTTON_MINUS, VPAD_BUTTON_PLUS,
        VPAD_BUTTON_RIGHT, VPAD_BUTTON_LEFT, VPAD_BUTTON_UP, VPAD_BUTTON_DOWN,
        VPAD_BUTTON_R, VPAD_BUTTON_L, VPAD_BUTTON_X, VPAD_BUTTON_Y,
    };
    std::uint32_t keys = 0;
    for (unsigned i = 0; i < 12; ++i)
        if ((buttons & mapping[i]) != 0) keys |= 1u << i;
    return keys;
}
} // namespace

void HIDManager::Update() {
    down_ = up_ = 0;
    VPADStatus sample{};
    VPADReadError error = VPAD_READ_UNINITIALIZED;
    const auto count = VPADRead(VPAD_CHAN_0, &sample, 1, &error);
    // Missing samples preserve held state without replaying button edges.
    if (error == VPAD_READ_NO_SAMPLES || error == VPAD_READ_BUSY) return;
    if (error != VPAD_READ_SUCCESS || count <= 0) {
        up_ = held_;
        held_ = x_ = y_ = touched_ = 0;
        return;
    }

    held_ = MapButtons(sample.hold);
    down_ = MapButtons(sample.trigger);
    up_ = MapButtons(sample.release);
    VPADTouchData touch{};
    VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, &touch, &sample.tpNormal);
    touched_ = (touch.touched != 0 && touch.validity == VPAD_VALID) ? 1u : 0u;
    x_ = y_ = 0;
    if (touched_ != 0) {
        x_ = std::min<std::uint32_t>(touch.x, 853u) * 319u / 853u;
        y_ = std::min<std::uint32_t>(touch.y, 479u) * 239u / 479u;
    }
}

std::uint32_t HIDManager::ReadRegister(std::uint32_t address, unsigned width) const {
    if ((width != 2 && width != 4) || address < kBase || address >= kEnd ||
        width > kEnd - address || (address & (width - 1u)) != 0)
        throw std::out_of_range("Invalid HID register read");
    const std::uint32_t registers[] = {held_, down_, up_, x_, y_, touched_, 0, 0, 0};
    const auto offset = address - kBase;
    const auto value = registers[offset / 4u] >> ((offset & 3u) * 8u);
    return width == 2 ? value & 0xFFFFu : value;
}

} // namespace wiiu
