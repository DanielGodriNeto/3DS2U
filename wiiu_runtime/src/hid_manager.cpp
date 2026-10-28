#include "hid_manager.hpp"

#include <vpad/input.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace wiiu {
namespace {
constexpr std::uint32_t kExtended = (1u << 14) | (1u << 15) | (0xFu << 24);
constexpr std::uint32_t kStickDirections = 0xFF000000u;
constexpr std::uint32_t MapButtons(std::uint32_t buttons, bool n3ds) noexcept {
    constexpr std::uint32_t mapping[] = {
        VPAD_BUTTON_A,     VPAD_BUTTON_B,    VPAD_BUTTON_MINUS, VPAD_BUTTON_PLUS,
        VPAD_BUTTON_RIGHT, VPAD_BUTTON_LEFT, VPAD_BUTTON_UP,    VPAD_BUTTON_DOWN,
        VPAD_BUTTON_R,     VPAD_BUTTON_L,    VPAD_BUTTON_X,     VPAD_BUTTON_Y,
    };
    std::uint32_t keys = 0;
    for (unsigned i = 0; i < 12; ++i)
        if ((buttons & mapping[i]) != 0)
            keys |= 1u << i;
    if (n3ds) {
        if ((buttons & VPAD_BUTTON_ZL) != 0)
            keys |= 1u << 14;
        if ((buttons & VPAD_BUTTON_ZR) != 0)
            keys |= 1u << 15;
    }
    return keys;
}
std::int16_t Axis(float value) {
    if (!std::isfinite(value))
        return 0;
    return static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 156.0f));
}
std::uint32_t Directions(std::int16_t x, std::int16_t y, unsigned shift) {
    const auto bits = (x > 40 ? 1u : 0u) | (x < -40 ? 2u : 0u) | (y > 40 ? 4u : 0u) | (y < -40 ? 8u : 0u);
    return bits << shift;
}
std::uint32_t Axes(std::int16_t x, std::int16_t y) {
    return static_cast<std::uint16_t>(x) | (std::uint32_t{static_cast<std::uint16_t>(y)} << 16);
}
void Put32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8u));
}
void Put64(std::span<std::uint8_t> bytes, std::size_t offset, std::uint64_t value) {
    Put32(bytes, offset, static_cast<std::uint32_t>(value));
    Put32(bytes, offset + 4, static_cast<std::uint32_t>(value >> 32));
}
} // namespace

void HIDManager::Update() {
    const auto previous = held_;
    down_ = up_ = 0;
    VPADStatus sample{};
    VPADReadError error = VPAD_READ_UNINITIALIZED;
    const auto count = VPADRead(VPAD_CHAN_0, &sample, 1, &error);
    // Missing samples preserve held state without replaying button edges.
    if (error == VPAD_READ_NO_SAMPLES || error == VPAD_READ_BUSY)
        return;
    if (error != VPAD_READ_SUCCESS || count <= 0) {
        up_ = held_;
        held_ = x_ = y_ = touched_ = 0;
        circle_x_ = circle_y_ = cstick_x_ = cstick_y_ = 0;
        Publish(previous);
        return;
    }

    held_ = MapButtons(sample.hold, is_new_3ds_);
    down_ = MapButtons(sample.trigger, is_new_3ds_);
    up_ = MapButtons(sample.release, is_new_3ds_);
    circle_x_ = Axis(sample.leftStick.x);
    circle_y_ = Axis(sample.leftStick.y);
    cstick_x_ = is_new_3ds_ ? Axis(sample.rightStick.x) : 0;
    cstick_y_ = is_new_3ds_ ? Axis(sample.rightStick.y) : 0;
    held_ |= Directions(circle_x_, circle_y_, 28);
    if (is_new_3ds_)
        held_ |= Directions(cstick_x_, cstick_y_, 24);
    down_ |= (~previous & held_) & kStickDirections;
    up_ |= (previous & ~held_) & kStickDirections;
    VPADTouchData touch{};
    VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, &touch, &sample.tpNormal);
    touched_ = (touch.touched != 0 && touch.validity == VPAD_VALID) ? 1u : 0u;
    x_ = y_ = 0;
    if (touched_ != 0) {
        x_ = std::min<std::uint32_t>(touch.x, 853u) * 319u / 853u;
        y_ = std::min<std::uint32_t>(touch.y, 479u) * 239u / 479u;
    }
    Publish(previous);
}

void HIDManager::Publish(std::uint32_t previous) {
    const auto tick = ++sequence_;
    const auto index = static_cast<std::uint32_t>((tick - 1u) & 7u);
    // HID PAD entries start at 0x28; touch entries at 0xA8 + 0x20.
    Put64(shared_, 0, tick);
    Put64(shared_, 8, tick - 1);
    const auto pad = 0x28u + index * 16u;
    Put32(shared_, pad, held_ & ~kExtended);
    Put32(shared_, pad + 4, (~previous & held_) & ~kExtended);
    Put32(shared_, pad + 8, (previous & ~held_) & ~kExtended);
    Put32(shared_, pad + 12, Axes(circle_x_, circle_y_));
    Put64(shared_, 0xA8, tick);
    Put64(shared_, 0xB0, tick - 1);
    const auto touch = 0xC8u + index * 8u;
    Put32(shared_, touch, x_ | (y_ << 16));
    Put32(shared_, touch + 4, touched_);
    Put32(shared_, 0x10, index);
    Put32(shared_, 0xB8, index);
    if (is_new_3ds_) {
        Put64(irrst_, 0, tick);
        Put64(irrst_, 8, tick - 1);
        const auto entry = 0x18u + index * 16u;
        Put32(irrst_, entry, held_ & kExtended);
        Put32(irrst_, entry + 4, (~previous & held_) & kExtended);
        Put32(irrst_, entry + 8, (previous & ~held_) & kExtended);
        Put32(irrst_, entry + 12, Axes(cstick_x_, cstick_y_));
        Put32(irrst_, 0x10, index);
    }
}

std::uint32_t HIDManager::ReadRegister(std::uint32_t address, unsigned width) const {
    if ((width != 1 && width != 2 && width != 4) || (address & (width - 1u)) != 0)
        throw std::out_of_range("Invalid HID register read");
    std::span<const std::uint8_t> bytes;
    std::uint32_t start{};
    if (address >= kSharedBase && address < kSharedBase + kSharedSize) {
        bytes = shared_;
        start = kSharedBase;
    } else if (address >= kIRRSTBase && address < kIRRSTBase + kIRRSTSize) {
        bytes = irrst_;
        start = kIRRSTBase;
    }
    if (!bytes.empty()) {
        const auto offset = address - start;
        if (width > bytes.size() - offset)
            throw std::out_of_range("HID shared-memory read outside block");
        std::uint32_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= std::uint32_t{bytes[offset + i]} << (i * 8u);
        return value;
    }
    if (address < kBase || address >= kEnd || width > kEnd - address)
        throw std::out_of_range("Invalid HID register read");
    const std::uint32_t registers[] = {held_,
                                       down_,
                                       up_,
                                       x_,
                                       y_,
                                       touched_,
                                       Axes(circle_x_, circle_y_),
                                       Axes(cstick_x_, cstick_y_),
                                       held_ & kExtended};
    const auto offset = address - kBase;
    const auto value = registers[offset / 4u] >> ((offset & 3u) * 8u);
    return width == 1 ? value & 0xFFu : width == 2 ? value & 0xFFFFu : value;
}

} // namespace wiiu
