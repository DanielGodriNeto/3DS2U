#pragma once

// Host stand-in for wiiu_runtime/include/svc_handler.hpp. It records every SVC id and writes a
// fixed result to r0, so the execution dump shows that the call ran.

#include "arm11_types.hpp"

#include <cstdint>
#include <vector>

namespace wiiu {

class SvcHandler {
public:
    static constexpr std::uint32_t kResultBase = 0x5A5A0000u;

    static inline std::vector<std::uint32_t> calls;

    static void Execute(arm11::CPUContext& cpu, std::uint32_t svc_id) {
        calls.push_back(svc_id);
        cpu.r[0] = kResultBase | svc_id;
    }
};

} // namespace wiiu
