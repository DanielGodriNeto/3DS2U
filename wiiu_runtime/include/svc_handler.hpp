#pragma once
#include <cstdint>
#include <memory>

namespace arm11 { struct CPUContext; }
namespace wiiu {
class WiiUMemory;

class SvcHandler {
public:
    using BlockRunner = bool (*)(void*, arm11::CPUContext&);
    enum class WaitState { None, Waiting, Signaled, TimedOut };
    static constexpr std::uint32_t kInvalidArgument = 0xD9000101u;
    static constexpr std::uint32_t kOutOfResources = 0xD9000102u;
    static constexpr std::uint32_t kNotSupported = 0xD9000103u;
    static constexpr std::uint32_t kInvalidHandle = 0xD9000104u;

    explicit SvcHandler(WiiUMemory& memory, BlockRunner runner = nullptr, void* opaque = nullptr);
    ~SvcHandler();
    SvcHandler(const SvcHandler&) = delete;
    SvcHandler& operator=(const SvcHandler&) = delete;
    static void Execute(arm11::CPUContext& cpu, std::uint32_t svc_id);
    [[nodiscard]] bool Ready(arm11::CPUContext& cpu);
    [[nodiscard]] WaitState GetWaitState(const arm11::CPUContext& cpu);
    // Worker waits occur outside the engine block mutex; the main thread polls Ready.
    void Park(arm11::CPUContext& cpu);
    void Shutdown() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace wiiu
