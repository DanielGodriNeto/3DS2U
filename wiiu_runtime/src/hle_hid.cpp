#include "hle_hid.hpp"

#include <vpad/input.h>

namespace wiiu::hle {
namespace {

constexpr auto BIT = [](unsigned bit) { return std::uint32_t{1} << bit; };

struct ButtonMapping {
    std::uint32_t vpad;
    std::uint32_t hid;
};

// VPAD and 3DS HID assign different bit positions to the same physical buttons.
constexpr ButtonMapping kButtons[] = {
    {VPAD_BUTTON_A,     BIT(0)},
    {VPAD_BUTTON_B,     BIT(1)},
    {VPAD_BUTTON_MINUS, BIT(2)},
    {VPAD_BUTTON_PLUS,  BIT(3)},
    {VPAD_BUTTON_RIGHT, BIT(4)},
    {VPAD_BUTTON_LEFT,  BIT(5)},
    {VPAD_BUTTON_UP,    BIT(6)},
    {VPAD_BUTTON_DOWN,  BIT(7)},
    {VPAD_BUTTON_R,     BIT(8)},
    {VPAD_BUTTON_L,     BIT(9)},
    {VPAD_BUTTON_X,    BIT(10)},
    {VPAD_BUTTON_Y,    BIT(11)},
};

constexpr std::uint32_t TranslateButtons(std::uint32_t buttons) noexcept {
    std::uint32_t keys = 0;
    for (const auto& mapping : kButtons) {
        if ((buttons & mapping.vpad) != 0) {
            keys |= mapping.hid;
        }
    }
    return keys;
}

} // namespace

void HLEHID::ScanInput() {
    down_keys = 0; // Trigger bits belong only to the current scan.
    VPADStatus vpad_data{};
    VPADReadError vpad_error = VPAD_READ_UNINITIALIZED;
    const auto samples = VPADRead(VPAD_CHAN_0, &vpad_data, 1, &vpad_error);

    // No new sample preserves held buttons without replaying a press edge.
    if (vpad_error == VPAD_READ_NO_SAMPLES || vpad_error == VPAD_READ_BUSY) {
        return;
    }
    // Failed/disconnected reads must not introduce random or stuck HID buttons.
    if (vpad_error != VPAD_READ_SUCCESS || samples <= 0) {
        current_keys = 0;
        return;
    }

    current_keys = TranslateButtons(vpad_data.hold);
    down_keys = TranslateButtons(vpad_data.trigger);
}

} // namespace wiiu::hle
