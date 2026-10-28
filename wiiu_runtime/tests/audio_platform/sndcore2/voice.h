#pragma once
#include <array>
#include <cstdint>
constexpr std::uint16_t AX_VOICE_FORMAT_ADPCM=0, AX_VOICE_FORMAT_LPCM16=10, AX_VOICE_FORMAT_LPCM8=25;
constexpr std::uint16_t AX_VOICE_LOOP_DISABLED=0, AX_VOICE_LOOP_ENABLED=1;
constexpr std::uint32_t AX_VOICE_STATE_STOPPED=0, AX_VOICE_STATE_PLAYING=1;
constexpr std::uint32_t AX_VOICE_TYPE_UNKNOWN=0, AX_VOICE_SRC_TYPE_LINEAR=1;
constexpr int AX_RESULT_SUCCESS=0, AX_VOICE_RATIO_RESULT_SUCCESS=0;
constexpr std::uint32_t AX_DEVICE_TYPE_TV=0, AX_DEVICE_TYPE_DRC=1;
struct AXVoiceOffsets {
    std::uint16_t dataType{}, loopingEnabled{};
    std::uint32_t loopOffset{}, endOffset{}, currentOffset{};
    const void* data{};
};
struct AXVoiceVeData { std::uint16_t volume{}; std::int16_t delta{}; };
struct AXVoiceSrc { std::uint32_t ratio{}; std::uint16_t currentOffsetFrac{}; std::int16_t lastSample[4]{}; };
struct AXVoiceDeviceBusMixData { std::uint16_t volume{}; std::int16_t delta{}; };
struct AXVoiceDeviceMixData { AXVoiceDeviceBusMixData bus[4]{}; };
struct AXVoiceAdpcm { std::int16_t coefficients[16]{}; std::uint16_t gain{}, predScale{}; std::int16_t prevSample[2]{}; };
struct AXVoiceAdpcmLoopData { std::uint16_t predScale{}; std::int16_t prevSample[2]{}; };
struct AXVoice {
    bool acquired{};
    std::uint32_t state{}, type{}, src_type{};
    AXVoiceOffsets offsets{};
    AXVoiceVeData envelope{};
    AXVoiceSrc src{};
    AXVoiceAdpcm adpcm{};
    AXVoiceAdpcmLoopData adpcm_loop{};
    float ratio{};
    unsigned mix_calls{}, begins{}, ends{};
    std::array<std::uint16_t,2> left_mix{}, right_mix{};
};
namespace ax_mock {
inline std::array<AXVoice,16> voices;
inline int fail_after = -1;
inline unsigned acquired{}, freed{};
inline void (*before_begin)(){};
}
inline AXVoice* AXAcquireVoice(std::uint32_t, void (*)(void*), void*) {
    if (ax_mock::fail_after >= 0 && ax_mock::acquired >= static_cast<unsigned>(ax_mock::fail_after)) return nullptr;
    for (auto& voice : ax_mock::voices) if (!voice.acquired) { voice = {}; voice.acquired = true; ++ax_mock::acquired; return &voice; }
    return nullptr;
}
inline void AXFreeVoice(AXVoice* voice) { voice->acquired = false; voice->state = 0; ++ax_mock::freed; }
inline int AXVoiceBegin(AXVoice* voice) { if (ax_mock::before_begin) ax_mock::before_begin(); ++voice->begins; return 0; }
inline int AXVoiceEnd(AXVoice* voice) { ++voice->ends; return 0; }
inline bool AXIsVoiceRunning(AXVoice* voice) { return voice->state == AX_VOICE_STATE_PLAYING; }
inline void AXSetVoiceState(AXVoice* voice, std::uint32_t state) { voice->state = state; }
inline void AXSetVoiceType(AXVoice* voice, std::uint32_t type) { voice->type = type; }
inline void AXSetVoiceOffsets(AXVoice* voice, AXVoiceOffsets* offsets) { voice->offsets = *offsets; }
inline void AXSetVoiceSrcType(AXVoice* voice, std::uint32_t type) { voice->src_type = type; }
inline void AXSetVoiceSrc(AXVoice* voice, AXVoiceSrc* src) { voice->src = *src; voice->ratio = static_cast<float>(src->ratio) / 65536.0f; }
inline int AXSetVoiceSrcRatio(AXVoice* voice, float ratio) { if (ratio > 16 || ratio <= 0) return -2; voice->ratio = ratio; return 0; }
inline void AXSetVoiceVe(AXVoice* voice, AXVoiceVeData* envelope) { voice->envelope = *envelope; }
inline int AXSetVoiceDeviceMix(AXVoice* voice, std::uint32_t type, std::uint32_t, AXVoiceDeviceMixData* mix) {
    ++voice->mix_calls; voice->left_mix.at(type) = mix[0].bus[0].volume;
    voice->right_mix.at(type) = mix[1].bus[0].volume; return 0;
}
inline void AXSetVoiceAdpcm(AXVoice* voice, AXVoiceAdpcm* state) { voice->adpcm = *state; }
inline void AXSetVoiceAdpcmLoop(AXVoice* voice, AXVoiceAdpcmLoopData* state) { voice->adpcm_loop = *state; }
