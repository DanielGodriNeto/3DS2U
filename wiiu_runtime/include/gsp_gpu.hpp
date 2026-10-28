#pragma once

#include <cstdint>

namespace wiiu {
class Pica200;
class WiiUMemory;
class DisplayTransfer;

// Command-stream entry point; guest GSP IPC/physical-address translation is separate.
class GSPGPU {
public:
    explicit GSPGPU(Pica200& pica) noexcept : pica_(pica) {}
    GSPGPU(Pica200& pica, DisplayTransfer& display) noexcept : pica_(pica), display_(&display) {}
    // Uses PICA parameter/header ordering, byte masks and word-indexed register IDs.
    // Throws on malformed packets; syntactic validation precedes all register writes.
    void ProcessCommandBuffer(WiiUMemory& memory, std::uint32_t cmd_buf_addr,
                              std::uint32_t size_bytes);
    enum class GXDialect { Native, MVPDisplayTransfer01 };
    // One fixed eight-word GX packet, distinct from the PICA register command stream.
    // Native: 0x01=PICA list, 0x03=DisplayTransfer. Legacy mode explicitly aliases 0x01.
    void ProcessGXCommand(WiiUMemory& memory, std::uint32_t packet_address,
                          GXDialect dialect = GXDialect::Native);

private:
    Pica200& pica_;
    DisplayTransfer* display_{};
};
} // namespace wiiu
