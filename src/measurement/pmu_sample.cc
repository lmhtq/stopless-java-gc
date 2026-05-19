// pmu_sample.cc — Phase 0 stub. Real PMU access wired in Phase 1.

#include "pmu_sample.h"

#include <chrono>
#include <cstdio>

namespace stopless {

namespace {
inline uint64_t now_ns() {
    using clk = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            clk::now().time_since_epoch()).count());
}
}  // namespace

PmuWindow::PmuWindow() {
    start_ = PmuSample{ now_ns(), 0, 0, 0 };
    // TODO(phase1): also open perf_event_open() / FVP-stat handles.
}

PmuWindow::~PmuWindow() = default;

PmuSample PmuWindow::sample() const {
    PmuSample now{ now_ns(), 0, 0, 0 };
    PmuSample delta{
        now.cycles - start_.cycles,
        now.instructions - start_.instructions,
        now.cap_traps - start_.cap_traps,
        now.gc_barrier_slot_cycles - start_.gc_barrier_slot_cycles,
    };
    return delta;
}

void append_pmu_json(const char* path, const PmuSample& s) {
    FILE* f = std::fopen(path, "a");
    if (!f) return;
    std::fprintf(f,
        "{\"cycles\":%llu,\"instructions\":%llu,"
        "\"cap_traps\":%llu,\"gc_barrier_slot_cycles\":%llu}\n",
        (unsigned long long)s.cycles,
        (unsigned long long)s.instructions,
        (unsigned long long)s.cap_traps,
        (unsigned long long)s.gc_barrier_slot_cycles);
    std::fclose(f);
}

}  // namespace stopless
