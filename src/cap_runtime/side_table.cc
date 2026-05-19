// side_table.cc — implementation of stopless::SideTable.
//
// All operations are relaxed-atomic on individual bytes. Mutator load
// barriers consult this; the GC mutates it under the existing ZGC phase
// transition (no new locking).

#include "side_table.h"

#include <cstring>
#include <cstdio>

namespace stopless {

SideTable::SideTable(size_t heap_bytes)
    : entries_(heap_bytes >> TABLE_SHIFT),
      table_(new std::atomic<uint8_t>[heap_bytes >> TABLE_SHIFT]),
      heap_base_(0)
{
    // Zero the table to STOPLESS_COLOR_GOOD.
    static_assert(STOPLESS_COLOR_GOOD == 0,
                  "color zero must mean GOOD for memset-init to work");
    for (size_t i = 0; i < entries_; ++i) {
        table_[i].store(0, std::memory_order_relaxed);
    }
}

SideTable::~SideTable() = default;

static inline uintptr_t to_addr(stopless_cap_t cap) {
#if STOPLESS_PURECAP
    return reinterpret_cast<uintptr_t>(static_cast<void*>(cap));
#else
    return reinterpret_cast<uintptr_t>(cap);
#endif
}

stopless_color_t SideTable::load(stopless_cap_t cap) const {
    uintptr_t addr = to_addr(cap);
    if (addr < heap_base_) return STOPLESS_COLOR_GOOD;
    size_t idx = (addr - heap_base_) >> TABLE_SHIFT;
    if (idx >= entries_) return STOPLESS_COLOR_GOOD;
    return static_cast<stopless_color_t>(
        table_[idx].load(std::memory_order_relaxed));
}

void SideTable::store(stopless_cap_t cap, stopless_color_t color) {
    uintptr_t addr = to_addr(cap);
    if (addr < heap_base_) return;
    size_t idx = (addr - heap_base_) >> TABLE_SHIFT;
    if (idx >= entries_) return;
    table_[idx].store(static_cast<uint8_t>(color),
                      std::memory_order_relaxed);
}

void SideTable::flip_region(stopless_region_t* /*region*/,
                            stopless_color_t   from,
                            stopless_color_t   to) {
    // TODO(phase1): region-scoped flip. Phase 0 implementation flips the
    // whole table; this is correct but inefficient and will be replaced
    // once stopless_region_t carries [begin,end) extents.
    for (size_t i = 0; i < entries_; ++i) {
        uint8_t expected = static_cast<uint8_t>(from);
        table_[i].compare_exchange_strong(
            expected,
            static_cast<uint8_t>(to),
            std::memory_order_relaxed);
    }
}

}  // namespace stopless
