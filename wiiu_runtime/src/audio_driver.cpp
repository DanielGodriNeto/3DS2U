#include "audio_driver.hpp"
#include "wiiu_memory.hpp"
#include <sndcore2/core.h>
#include <sndcore2/voice.h>
#include <coreinit/cache.h>
#include <coreinit/debug.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/mutex.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>

namespace wiiu {
namespace {
std::atomic<std::uint32_t> audio_frames{};
void AudioFrame() { audio_frames.fetch_add(1, std::memory_order_relaxed); }
constexpr std::uint32_t kBufferLimit = 4u * 1024u * 1024u;
constexpr std::uint32_t kRetiredLimit = 16u * 1024u * 1024u;
// Covers the default single AX pipeline plus in-flight DSP work conservatively.
constexpr std::uint32_t kRetireFrames = 6;
struct FreeBuffer { void operator()(std::uint8_t* p) const noexcept { if (p) MEMFreeToDefaultHeap(p); } };
using Buffer = std::unique_ptr<std::uint8_t, FreeBuffer>;
struct Lock {
    OSMutex& mutex;
    explicit Lock(OSMutex& value) : mutex(value) { OSLockMutex(&mutex); }
    ~Lock() { OSUnlockMutex(&mutex); }
};
struct VoiceUpdate {
    AXVoice* voice;
    explicit VoiceUpdate(AXVoice* v) : voice(v) { AXVoiceBegin(v); }
    ~VoiceUpdate() { AXVoiceEnd(voice); }
};
} // namespace

struct AudioDriver::Impl {
    struct Channel {
        AXVoice* voice{};
        Buffer data;
        std::uint32_t allocation_size{}, rate = 32000;
        float volume = 1.0f, pitch = 1.0f;
        std::optional<ADPCMParameters> adpcm;
        std::array<std::uint32_t, 7> registers{0x8000, 0x10000, 0, 0, 32000, 1, 0};
    };
    struct Retired { Buffer data; std::uint32_t size, frame; };
    WiiUMemory& memory;
    OSMutex mutex{};
    std::array<Channel, kVoiceCount> channels;
    std::vector<Retired> retired;
    std::uint32_t retired_bytes{};
    bool initialized{}, owns_ax{}, registered{};
    explicit Impl(WiiUMemory& mem) : memory(mem) { OSInitMutex(&mutex); }
    Channel& Get(std::uint32_t channel) {
        if (channel >= channels.size()) throw std::out_of_range("Invalid AX channel");
        return channels[channel];
    }
    float Ratio(const Channel& channel, std::uint32_t rate, float pitch) const {
        (void)channel;
        const auto renderer_rate = AXGetInputSamplesPerSec();
        const auto ratio = static_cast<float>(rate) * pitch / static_cast<float>(renderer_rate);
        if (renderer_rate == 0 || !std::isfinite(ratio) || ratio < 1.0f / 65536.0f || ratio > 16.0f)
            throw std::invalid_argument("AX sample-rate ratio outside supported range");
        return ratio;
    }
    void Retire(Channel& channel) {
        if (!channel.data) return;
        if (channel.allocation_size > kRetiredLimit - retired_bytes)
            throw std::runtime_error("AX pending DMA buffer budget exhausted; retry after Pump");
        retired.push_back({{}, channel.allocation_size, audio_frames.load(std::memory_order_relaxed)});
        retired.back().data = std::move(channel.data);
        retired_bytes += channel.allocation_size; channel.allocation_size = 0;
    }
};

AudioDriver::AudioDriver(WiiUMemory& memory) : impl_(std::make_unique<Impl>(memory)) {}
AudioDriver::~AudioDriver() noexcept { Shutdown(); }

bool AudioDriver::Initialize() {
    auto& state = *impl_;
    Lock lock(state.mutex);
    if (state.initialized) return true;
    if (AXIsInit()) { OSReport("[3DS2U Audio] AX already has an owner\n"); return false; }
    AXInit(); state.owns_ax = true;
    if (!AXIsInit()) { Shutdown(); return false; }
    if (AXRegisterAppFrameCallback(AudioFrame) != AX_RESULT_SUCCESS) { Shutdown(); return false; }
    state.registered = true;
    for (auto& channel : state.channels) {
        channel.voice = AXAcquireVoice(31, nullptr, nullptr);
        if (!channel.voice) { Shutdown(); return false; }
        AXSetVoiceState(channel.voice, AX_VOICE_STATE_STOPPED);
        std::array<AXVoiceDeviceMixData, 6> mix{};
        mix[0].bus[0].volume = mix[1].bus[0].volume = 0x8000;
        if (AXSetVoiceDeviceMix(channel.voice, AX_DEVICE_TYPE_TV, 0, mix.data()) != AX_RESULT_SUCCESS ||
            AXSetVoiceDeviceMix(channel.voice, AX_DEVICE_TYPE_DRC, 0, mix.data()) != AX_RESULT_SUCCESS) {
            Shutdown(); return false;
        }
    }
    state.initialized = true;
    OSReport("[3DS2U Audio] AX initialized with 16 mono voices\n"); return true;
}

void AudioDriver::Shutdown() noexcept {
    if (!impl_) return;
    auto& state = *impl_;
    Lock lock(state.mutex);
    if (state.owns_ax) {
        for (auto& channel : state.channels) if (channel.voice) {
            AXSetVoiceState(channel.voice, AX_VOICE_STATE_STOPPED);
            AXFreeVoice(channel.voice); channel.voice = nullptr;
        }
        if (state.registered) AXDeregisterAppFrameCallback(AudioFrame);
        // AXQuit stops DSP consumption before any DMA storage is released.
        AXQuit();
    }
    for (auto& channel : state.channels) { channel.data.reset(); channel.allocation_size = 0; }
    state.retired.clear(); state.retired_bytes = 0;
    state.initialized = state.owns_ax = state.registered = false;
}

void AudioDriver::Pump() noexcept {
    auto& state = *impl_;
    Lock lock(state.mutex);
    const auto frame = audio_frames.load(std::memory_order_relaxed);
    for (auto it = state.retired.begin(); it != state.retired.end();) {
        if (frame - it->frame >= kRetireFrames) {
            state.retired_bytes -= it->size; it = state.retired.erase(it);
        } else ++it;
    }
}

void AudioDriver::SetADPCMParameters(std::uint32_t channel, const ADPCMParameters& parameters) {
    Lock lock(impl_->mutex);
    impl_->Get(channel).adpcm = parameters;
}

void AudioDriver::PlayVoice(std::uint32_t index, std::uint32_t address, std::uint32_t rate,
                            std::uint16_t format, std::uint32_t size, bool loop) {
    auto& state = *impl_; auto& channel = state.Get(index);
    Lock lock(state.mutex);
    if (!state.initialized) throw std::logic_error("AX is not initialized");
    if (format > 2 || size == 0 || size > kBufferLimit || rate < 1000 || rate > 192000 ||
        (format == 1 && ((address | size) & 1u) != 0) ||
        (format == 2 && ((address & 7u) != 0 || (size & 7u) != 0 || !channel.adpcm)))
        throw std::invalid_argument("Invalid audio format, alignment, size or ADPCM metadata");
    const auto ratio = state.Ratio(channel, rate, channel.pitch);
    const auto* source = static_cast<const std::uint8_t*>(state.memory.GetPointer(address, size));
    if (format == 2) for (std::uint32_t i = 0; i < size; i += 8)
        if ((source[i] >> 4) > 7) throw std::invalid_argument("Invalid DSP ADPCM predictor");
    Pump();
    const auto allocation = (size + 63u) & ~31u;
    Buffer upload(static_cast<std::uint8_t*>(MEMAllocFromDefaultHeapEx(allocation, 0x100)));
    if (!upload) throw std::bad_alloc{};
    std::memset(upload.get(), 0, allocation);
    DCStoreRange(const_cast<std::uint8_t*>(source), size);
    for (std::uint32_t i = 0; i < size; ++i) {
        if (format == 0) upload.get()[i] = source[i] ^ 0x80u;
        else if (format == 1) upload.get()[i] = source[i ^ 1u];
        else upload.get()[i] = source[i];
    }
    DCStoreRange(upload.get(), allocation);
    const auto retired_count = state.retired.size();
    state.Retire(channel);
    {
        VoiceUpdate update(channel.voice);
        AXSetVoiceState(channel.voice, AX_VOICE_STATE_STOPPED);
        AXSetVoiceType(channel.voice, AX_VOICE_TYPE_UNKNOWN);
        AXVoiceOffsets offsets{};
        offsets.dataType = format == 0 ? AX_VOICE_FORMAT_LPCM8 : format == 1 ? AX_VOICE_FORMAT_LPCM16 : AX_VOICE_FORMAT_ADPCM;
        offsets.loopingEnabled = loop ? AX_VOICE_LOOP_ENABLED : AX_VOICE_LOOP_DISABLED;
        offsets.data = upload.get();
        offsets.currentOffset = offsets.loopOffset = format == 2 ? 2u : 0u;
        offsets.endOffset = (format == 0 ? size : format == 1 ? size / 2u : size * 2u) - 1u;
        AXSetVoiceOffsets(channel.voice, &offsets);
        if (format == 2) {
            AXVoiceAdpcm adpcm{}; AXVoiceAdpcmLoopData loop_data{};
            for (unsigned i = 0; i < 16; ++i) adpcm.coefficients[i] = channel.adpcm->coefficients[i];
            for (unsigned i = 0; i < 2; ++i) {
                adpcm.prevSample[i] = channel.adpcm->history[i];
                loop_data.prevSample[i] = channel.adpcm->loop_history[i];
            }
            adpcm.predScale = loop_data.predScale = source[0];
            AXSetVoiceAdpcm(channel.voice, &adpcm); AXSetVoiceAdpcmLoop(channel.voice, &loop_data);
        }
        AXSetVoiceSrcType(channel.voice, AX_VOICE_SRC_TYPE_LINEAR);
        AXVoiceSrc src{}; src.ratio = static_cast<std::uint32_t>(ratio * 65536.0f);
        AXSetVoiceSrc(channel.voice, &src);
        AXVoiceVeData envelope{static_cast<std::uint16_t>(channel.volume * 32768.0f), 0};
        AXSetVoiceVe(channel.voice, &envelope);
        channel.data = std::move(upload); channel.allocation_size = allocation; channel.rate = rate;
        channel.registers[2] = address; channel.registers[3] = size; channel.registers[4] = rate;
        channel.registers[5] = format; channel.registers[6] = loop ? 3u : 1u;
        AXSetVoiceState(channel.voice, AX_VOICE_STATE_PLAYING);
    }
    // Timestamp after submission; preemption before stopping the old voice cannot age it early.
    if (state.retired.size() != retired_count)
        state.retired.back().frame = audio_frames.load(std::memory_order_relaxed);
}

void AudioDriver::SetVolume(std::uint32_t index, float volume) {
    Lock lock(impl_->mutex);
    auto& channel = impl_->Get(index);
    if (!std::isfinite(volume) || volume < 0 || volume > 1) throw std::invalid_argument("Invalid voice volume");
    channel.volume = volume; channel.registers[0] = static_cast<std::uint32_t>(volume * 32768.0f);
    if (channel.voice) {
        VoiceUpdate update(channel.voice);
        AXVoiceVeData envelope{static_cast<std::uint16_t>(channel.registers[0]), 0};
        AXSetVoiceVe(channel.voice, &envelope);
    }
}
void AudioDriver::SetPitch(std::uint32_t index, float multiplier) {
    Lock lock(impl_->mutex);
    auto& channel = impl_->Get(index);
    if (!std::isfinite(multiplier) || multiplier <= 0 || multiplier > 16)
        throw std::invalid_argument("Invalid voice pitch");
    if (channel.voice) {
        const auto ratio = impl_->Ratio(channel, channel.rate, multiplier);
        VoiceUpdate update(channel.voice);
        if (AXSetVoiceSrcRatio(channel.voice, ratio) != AX_VOICE_RATIO_RESULT_SUCCESS)
            throw std::invalid_argument("AX rejected the voice pitch");
    }
    channel.pitch = multiplier; channel.registers[1] = static_cast<std::uint32_t>(multiplier * 65536.0f);
}
void AudioDriver::StopVoice(std::uint32_t index) {
    Lock lock(impl_->mutex);
    auto& channel = impl_->Get(index);
    if (channel.voice) { VoiceUpdate update(channel.voice); AXSetVoiceState(channel.voice, AX_VOICE_STATE_STOPPED); }
    // Keep stopped storage owned until replacement or shutdown.
    channel.registers[6] = 0;
}
void AudioDriver::StopAll() noexcept {
    Lock lock(impl_->mutex);
    for (std::uint32_t i = 0; i < kVoiceCount; ++i) StopVoice(i);
}
bool AudioDriver::IsPlaying(std::uint32_t index) const {
    Lock lock(impl_->mutex);
    auto& channel = impl_->Get(index);
    return channel.voice && AXIsVoiceRunning(channel.voice);
}

bool AudioDriver::ReadMMIO(std::uint32_t address, unsigned width, std::uint32_t& value) const {
    Lock lock(impl_->mutex);
    if (address < kChannelBase || address >= kChannelBase + kVoiceCount * kChannelStride) return false;
    if ((width != 2 && width != 4) || address % width != 0) throw std::out_of_range("Unaligned audio register");
    const auto relative = address - kChannelBase;
    const auto channel = relative / kChannelStride, reg = (relative % kChannelStride) / 4;
    value = reg == 7 ? (IsPlaying(channel) ? 1u : 0u) : impl_->Get(channel).registers[reg];
    value = (value >> ((address & 3u) * 8u)) & (width == 2 ? 0xFFFFu : 0xFFFFFFFFu);
    return true;
}
bool AudioDriver::WriteMMIO(std::uint32_t address, std::uint32_t value, unsigned width) {
    Lock lock(impl_->mutex);
    std::uint32_t previous{};
    if (!ReadMMIO(address & ~3u, 4, previous)) return false;
    if ((width != 2 && width != 4) || address % width != 0) throw std::out_of_range("Unaligned audio register");
    const auto channel = (address - kChannelBase) / kChannelStride;
    const auto reg = ((address - kChannelBase) % kChannelStride) / 4u;
    const auto shift = (address & 3u) * 8u;
    const auto mask = width == 2 ? 0xFFFFu << shift : 0xFFFFFFFFu;
    value = (previous & ~mask) | ((value << shift) & mask);
    auto& c = impl_->Get(channel);
    if (reg == 0) {
        if (value > 0x8000u) throw std::invalid_argument("Invalid audio Q15 volume");
        SetVolume(channel, static_cast<float>(value) / 32768.0f);
    } else if (reg == 1) SetPitch(channel, static_cast<float>(value) / 65536.0f);
    else if (reg == 6) {
        if ((value & ~7u) != 0 || ((value & 5u) == 5u)) throw std::invalid_argument("Invalid audio control");
        if ((value & 1u) != 0) {
            if (c.registers[5] > 2) throw std::invalid_argument("Invalid audio format register");
            PlayVoice(channel, c.registers[2], c.registers[4], static_cast<std::uint16_t>(c.registers[5]),
                      c.registers[3], (value & 2u) != 0);
        } else StopVoice(channel);
    } else if (reg != 7) c.registers[reg] = value;
    return true;
}
} // namespace wiiu
