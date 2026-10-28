#include "gsp_gpu.hpp"
#include "pica200.hpp"
#include "wiiu_memory.hpp"

#include <stdexcept>

namespace wiiu {
namespace {
constexpr std::uint32_t kFinalize = 0x0010u;

template<class Write>
void WalkPackets(WiiUMemory& memory, std::uint32_t base, std::uint32_t size, Write write) {
    for (std::uint32_t cursor = 0; cursor < size;) {
        if (size - cursor < 8) throw std::invalid_argument("Truncated PICA command header");
        // Read32 already converts FCRAM LE words to native PPC numeric values.
        const auto first = memory.Read32(base + cursor);
        const auto header = memory.Read32(base + cursor + 4);
        const auto reg = header & 0xFFFFu;
        const auto mask = (header >> 16) & 0xFu;
        const auto extra = (header >> 20) & 0xFFu;
        const bool consecutive = (header & 0x80000000u) != 0;
        const auto packet_bytes = (2u + extra + (extra & 1u)) * 4u;
        if ((header & 0x70000000u) != 0 || packet_bytes > size - cursor ||
            (consecutive && reg + extra > 0xFFFFu)) {
            throw std::invalid_argument("Invalid PICA command size/header/register range");
        }
        for (std::uint32_t i = 0; i <= extra; ++i) {
            const auto id = reg + (consecutive ? i : 0u);
            const auto value = i == 0 ? first : memory.Read32(base + cursor + 4u + i * 4u);
            if (mask == 0) continue;
            write(Pica200::kMMIOBase + id * 4u, value, mask);
            // libctru may append a second FINALIZE for 16-byte submission alignment.
            if (id == kFinalize) return;
        }
        cursor += packet_bytes;
    }
}
} // namespace

void GSPGPU::ProcessCommandBuffer(WiiUMemory& memory, std::uint32_t address,
                                  std::uint32_t size) {
    if (size == 0) return;
    if ((address & 7u) != 0 || (size & 7u) != 0 ||
        std::uint64_t{address} + size > (std::uint64_t{1} << 32)) {
        throw std::invalid_argument("PICA buffer requires aligned nonwrapping 8-byte packets");
    }
    // Validate the entire advertised guest range, even when FINALIZE occurs early.
    (void)memory.GetPointer(address, size);
    WalkPackets(memory, address, size, [](auto, auto, auto) {});
    pica_.BeginCommandList(memory);
    try {
        WalkPackets(memory, address, size, [&](auto target, auto value, auto byte_mask) {
            pica_.WriteMMIO(target, value, static_cast<std::uint8_t>(byte_mask));
        });
        if (pica_.EndCommandList()) memory.RequestFrame();
    } catch (...) {
        pica_.CancelCommandList();
        throw;
    }
}
} // namespace wiiu
