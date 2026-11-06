#include "progress_sink.hpp"

#include <cstddef>
#include <ostream>
#include <string_view>

namespace arm11 {
namespace {

// emit-units prints at most one line per 5% step (20 lines per run at most).
constexpr std::size_t kPercentPerStep = 5;

// Printable ASCII only, so a path in a reason cannot break the one-line format.
std::string Sanitize(std::string_view text) {
    std::string clean;
    clean.reserve(text.size());
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        clean.push_back(byte >= 0x20 && byte <= 0x7E ? c : '?');
    }
    return clean;
}

std::size_t PercentOf(std::size_t completed, std::size_t total) {
    return total == 0 ? 100 : completed * 100 / total;
}

// Writes one line per event. Throttling state is per sink, so each run starts fresh.
class LineSink {
public:
    explicit LineSink(std::ostream& out) : out_{&out} {}

    void operator()(const ProgressEvent& event) {
        if (event.failed) {
            *out_ << "[3DS2U] progress FAILED phase=" << event.phase
                  << " reason=" << Sanitize(event.reason) << '\n';
            return;
        }
        if (event.phase == "emit-units" && !ReachedNewStep(event)) return;
        *out_ << "[3DS2U] progress phase=" << event.phase << ' ' << event.completed << '/'
              << event.total << " units, " << event.blocks_completed << '/' << event.blocks_total
              << " blocks (" << PercentOf(event.completed, event.total) << "%)\n";
    }

private:
    // Bucket 0 counts as already printed, so the first step is 5%.
    bool ReachedNewStep(const ProgressEvent& event) {
        if (event.phase != last_phase_) {
            last_phase_ = std::string{event.phase};
            last_step_ = 0;
        }
        const auto step = PercentOf(event.completed, event.total) / kPercentPerStep;
        if (step <= last_step_) return false;
        last_step_ = step;
        return true;
    }

    std::ostream* out_;
    std::string last_phase_;
    std::size_t last_step_{};
};

} // namespace

ProgressSink MakeLineProgressSink(std::ostream& out) {
    return ProgressSink{LineSink{out}};
}

} // namespace arm11
