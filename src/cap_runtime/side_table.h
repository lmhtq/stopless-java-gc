// side_table.h — internal storage for object color (Phase 1).
//
// ZGC encodes color in the top bits of a 64-bit oop. On CHERI we can't do
// that (high bits are real address bits), so color lives in a shadow
// bitmap keyed by object address. One byte per object-alignment unit
// (typically 8 bytes), so overhead is ~12.5% of the heap. We can shrink
// this to nibbles in a Phase 2 refinement.
//
// Internal class; not part of the public ABI.

#pragma once

#include "cap_runtime.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace stopless {

class SideTable {
public:
    explicit SideTable(size_t heap_bytes);
    ~SideTable();

    stopless_color_t load(stopless_cap_t cap) const;
    void             store(stopless_cap_t cap, stopless_color_t color);

    void flip_region(stopless_region_t* region,
                     stopless_color_t   from,
                     stopless_color_t   to);

    size_t entry_count() const { return entries_; }

private:
    // One byte per object-alignment unit (8B). The whole table is allocated
    // up front so that the per-load access is a single shifted load with no
    // pointer chasing.
    static constexpr size_t OBJ_ALIGN_LOG2 = 3;  // 8-byte alignment
    static constexpr size_t TABLE_SHIFT    = OBJ_ALIGN_LOG2;

    size_t                       entries_;
    std::unique_ptr<std::atomic<uint8_t>[]> table_;

    // For host builds we synthesize a fake base so address-to-index is
    // stable in unit tests. On Morello this is the cap.base of the
    // heap root.
    uintptr_t                    heap_base_;
};

}  // namespace stopless
