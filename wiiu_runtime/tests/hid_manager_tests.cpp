#include "hid_manager.hpp"
#include <vpad/input.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
VPADStatus sample{};
VPADReadError error = VPAD_READ_SUCCESS;
std::int32_t samples = 1;
unsigned polls = 0;
void Check(bool condition) { if (!condition) throw std::runtime_error("HID test failed"); }
}
std::int32_t VPADRead(VPADChan channel, VPADStatus* result, std::uint32_t count,
                      VPADReadError* result_error) {
    Check(channel == VPAD_CHAN_0 && count == 1);
    ++polls;
    *result = sample;
    *result_error = error;
    return samples;
}
void VPADGetTPCalibratedPointEx(VPADChan, VPADTouchPadResolution resolution,
                               VPADTouchData* result, const VPADTouchData* raw) {
    Check(resolution == VPAD_TP_854X480);
    *result = *raw;
}
int main() {
    wiiu::HIDManager hid;
    constexpr auto base = wiiu::HIDManager::kBase;
    constexpr std::uint32_t buttons[] = {VPAD_BUTTON_A, VPAD_BUTTON_B,
        VPAD_BUTTON_MINUS, VPAD_BUTTON_PLUS, VPAD_BUTTON_RIGHT, VPAD_BUTTON_LEFT,
        VPAD_BUTTON_UP, VPAD_BUTTON_DOWN, VPAD_BUTTON_R, VPAD_BUTTON_L,
        VPAD_BUTTON_X, VPAD_BUTTON_Y};
    for (unsigned i = 0; i < 12; ++i) {
        sample = {};
        sample.hold = sample.trigger = sample.release = buttons[i];
        hid.Update();
        for (unsigned offset : {0u, 4u, 8u})
            Check(hid.ReadRegister(base + offset) == (1u << i));
        Check(hid.ReadRegister(base, 2) == (1u << i));
        Check(hid.ReadRegister(base + 2, 2) == 0);
    }
    sample = {};
    sample.tpNormal = {853, 479, 1, VPAD_VALID};
    hid.Update();
    Check(hid.ReadRegister(base + 12) == 319 && hid.ReadRegister(base + 16) == 239);
    Check(hid.ReadRegister(base + 20) == 1);
    sample.tpNormal = {65535, 65535, 1, VPAD_VALID};
    hid.Update();
    Check(hid.ReadRegister(base + 12) == 319 && hid.ReadRegister(base + 16) == 239);
    sample.tpNormal = {0, 0, 1, VPAD_VALID};
    hid.Update();
    Check(hid.ReadRegister(base + 12) == 0 && hid.ReadRegister(base + 16) == 0);
    for (std::uint16_t validity : {std::uint16_t{1}, std::uint16_t{2}}) {
        sample.tpNormal = {300, 200, 1, validity};
        hid.Update();
        Check(hid.ReadRegister(base + 20) == 0 && hid.ReadRegister(base + 12) == 0);
    }
    sample.hold = sample.trigger = VPAD_BUTTON_A;
    hid.Update();
    for (auto missing : {VPAD_READ_NO_SAMPLES, VPAD_READ_BUSY}) {
        error = missing;
        hid.Update();
        Check(hid.ReadRegister(base) == 1 && hid.ReadRegister(base + 4) == 0);
    }
    error = VPAD_READ_INVALID_CONTROLLER;
    hid.Update();
    Check(hid.ReadRegister(base) == 0 && hid.ReadRegister(base + 8) == 1);
    hid.Update();
    Check(hid.ReadRegister(base + 8) == 0);
    error = VPAD_READ_SUCCESS;
    samples = 0;
    hid.Update();
    Check(hid.ReadRegister(base) == 0);
    for (auto address : {base - 4, base + 1, base + 0x24}) {
        bool rejected = false;
        try { (void)hid.ReadRegister(address); }
        catch (const std::out_of_range&) { rejected = true; }
        Check(rejected);
    }
    for (unsigned offset : {24u, 28u, 32u}) Check(hid.ReadRegister(base + offset) == 0);
    Check(polls > 12);
    std::cout << "HID mapping, touch, error and register tests passed.\n";
}
