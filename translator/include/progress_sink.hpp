#pragma once

#include <cstddef>
#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>

namespace arm11 {

struct ProgressEvent {
    std::string_view phase;
    std::size_t completed{};
    std::size_t total{};
    std::size_t blocks_completed{};
    std::size_t blocks_total{};
    bool failed{};
    std::string reason;
};

using ProgressSink = std::function<void(const ProgressEvent&)>;

// Line-per-event sink on out. Throttles emit-units to one line per 5% step, prints
// FAILED lines with a sanitized reason, and is defined in progress_sink.cpp.
ProgressSink MakeLineProgressSink(std::ostream& out);

} // namespace arm11
