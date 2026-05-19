// pmu_sample.h — perf-counter sampling used to measure the per-load
// barrier cost on Morello FVP.
//
// Phase 0 carries header-only stubs; Phase 1 wires up real PMU access
// via Linux perf_event_open (on hybrid CHERI Linux) or via Morello FVP's
// statistical profiler tap (on CheriBSD).

#pragma once

#include <cstdint>
#include <cstddef>

namespace stopless {

struct PmuSample {
    uint64_t cycles;
    uint64_t instructions;
    uint64_t cap_traps;
    uint64_t gc_barrier_slot_cycles;  // CHERI cap-load events, if available
};

// RAII window — records counters at construction, computes delta on destruction.
class PmuWindow {
public:
    PmuWindow();
    ~PmuWindow();

    PmuSample sample() const;

private:
    PmuSample start_;
};

// Best-effort write of `sample` into a JSON line at `path` (appending).
void append_pmu_json(const char* path, const PmuSample& sample);

}  // namespace stopless
