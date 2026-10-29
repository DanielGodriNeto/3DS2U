#pragma once
#include <cstdint>
enum VPADChan { VPAD_CHAN_0 };
enum VPADReadError { VPAD_READ_SUCCESS = 0, VPAD_READ_NO_SAMPLES = -1,
    VPAD_READ_INVALID_CONTROLLER = -2, VPAD_READ_BUSY = -4, VPAD_READ_UNINITIALIZED = -5 };
enum VPADTouchPadResolution { VPAD_TP_854X480 = 2 };
enum { VPAD_VALID = 0, VPAD_INVALID_X = 1, VPAD_INVALID_Y = 2 };
enum : std::uint32_t {
    VPAD_BUTTON_A = 0x8000, VPAD_BUTTON_B = 0x4000,
    VPAD_BUTTON_X = 0x2000, VPAD_BUTTON_Y = 0x1000,
    VPAD_BUTTON_LEFT = 0x800, VPAD_BUTTON_RIGHT = 0x400,
    VPAD_BUTTON_UP = 0x200, VPAD_BUTTON_DOWN = 0x100,
    VPAD_BUTTON_L = 0x20, VPAD_BUTTON_R = 0x10,
    VPAD_BUTTON_PLUS = 8, VPAD_BUTTON_MINUS = 4,
    VPAD_BUTTON_ZL = 0x80, VPAD_BUTTON_ZR = 0x40,
};
struct VPADTouchData { std::uint16_t x{}, y{}, touched{}, validity{}; };
struct VPADVec2D { float x{}, y{}; };
struct VPADStatus {
    std::uint32_t hold{}, trigger{}, release{};
    VPADVec2D leftStick{}, rightStick{};
    VPADTouchData tpNormal{};
};
std::int32_t VPADRead(VPADChan, VPADStatus*, std::uint32_t, VPADReadError*);
void VPADGetTPCalibratedPointEx(VPADChan, VPADTouchPadResolution,
                               VPADTouchData*, const VPADTouchData*);
