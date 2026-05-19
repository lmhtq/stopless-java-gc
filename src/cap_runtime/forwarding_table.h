// forwarding_table.h — cap-aware forwarding for ZGC's relocation phase.
//
// Maps (from_cap.address) → (to_cap). The to-cap is a real Morello
// capability with bounds set to the destination region's object extent.
// On host builds the cap is just a void*; the address-based key works
// identically.
//
// Concurrency model: relocate-worker threads INSTALL; mutator load
// barriers LOOKUP. Install is single-writer per slot (per ZGC's
// existing relocation invariants); lookup is wait-free.

#pragma once

#include "cap_runtime.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace stopless {

class ForwardingTable {
public:
    explicit ForwardingTable(size_t heap_bytes);
    ~ForwardingTable();

    void           install(stopless_cap_t from, stopless_cap_t to);
    stopless_cap_t lookup(stopless_cap_t from) const;

    void clear_region(stopless_region_t* region);

    size_t installed_count() const {
        return installed_.load(std::memory_order_relaxed);
    }

private:
    // Open-addressed hash, linear probing. Sized to ~1.5× the largest
    // expected concurrently-evacuating object population. For a region
    // of 4 MiB with 32-byte average object size, we need ~128k slots
    // per region; we provision conservatively at heap-construction time.
    struct Slot {
        std::atomic<uintptr_t> from_addr;   // 0 = empty
        stopless_cap_t         to_cap;
    };

    size_t                  slots_count_;
    std::unique_ptr<Slot[]> slots_;
    std::atomic<size_t>     installed_{0};
};

}  // namespace stopless
