#pragma once
#include "voice.h"
namespace ax_mock {
inline bool initialized{};
inline void (*frame_callback)(){};
inline unsigned quits{};
inline bool fail_registration{};
inline void Tick(unsigned frames) { while (frames-- != 0) if (frame_callback) frame_callback(); }
}
inline void AXInit() { ax_mock::initialized = true; }
inline bool AXIsInit() { return ax_mock::initialized; }
inline void AXQuit() { ax_mock::initialized = false; ++ax_mock::quits; }
inline int AXRegisterAppFrameCallback(void (*callback)()) {
    if (ax_mock::fail_registration) return -1;
    ax_mock::frame_callback = callback; return 0;
}
inline int AXDeregisterAppFrameCallback(void (*)()) { ax_mock::frame_callback = nullptr; return 0; }
inline std::uint32_t AXGetInputSamplesPerSec() { return 32000; }
