#pragma once

#include <cstdint>

namespace wiiu {
class Pica200;
class WiiUMemory;

// Command-stream entry point; guest GSP IPC/physical-address translation is separate.
class GSPGPU {
public:
    explicit GSPGPU(Pica200& pica) noexcept : pica_(pica) {}
    // Uses PICA parameter/header ordering, byte masks and word-indexed register IDs.
    // Throws on malformed packets; syntactic validation precedes all register writes.
    void ProcessCommandBuffer(WiiUMemory& memory, std::uint32_t cmd_buf_addr,
                              std::uint32_t size_bytes);

private:
    Pica200& pica_;
};
} // namespace wiiu
