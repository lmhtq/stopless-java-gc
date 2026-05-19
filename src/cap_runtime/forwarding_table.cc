// forwarding_table.cc — implementation of stopless::ForwardingTable.

#include "forwarding_table.h"

#include <cstring>

namespace stopless {

static inline uintptr_t cap_addr(stopless_cap_t cap) {
#if STOPLESS_PURECAP
    return reinterpret_cast<uintptr_t>(static_cast<void*>(cap));
#else
    return reinterpret_cast<uintptr_t>(cap);
#endif
}

// Bit-mix hash, FxHash-like; good enough for cap addresses which are
// already 8-byte aligned.
static inline size_t hash_addr(uintptr_t a, size_t modulus) {
    a ^= a >> 33;
    a *= 0xff51afd7ed558ccdULL;
    a ^= a >> 33;
    return static_cast<size_t>(a) & (modulus - 1);
}

ForwardingTable::ForwardingTable(size_t heap_bytes) {
    // Provision one slot per 64 bytes of heap as a coarse upper bound on
    // simultaneously-forwarded objects. Round up to a power of two for
    // mask-based modulus.
    size_t target = heap_bytes / 64;
    if (target < 64) target = 64;
    size_t pow2 = 1;
    while (pow2 < target) pow2 <<= 1;
    slots_count_ = pow2;
    slots_.reset(new Slot[slots_count_]);
    for (size_t i = 0; i < slots_count_; ++i) {
        slots_[i].from_addr.store(0, std::memory_order_relaxed);
        slots_[i].to_cap = stopless_cap_t{};
    }
}

ForwardingTable::~ForwardingTable() = default;

void ForwardingTable::install(stopless_cap_t from, stopless_cap_t to) {
    uintptr_t key = cap_addr(from);
    if (key == 0) return;
    size_t idx = hash_addr(key, slots_count_);
    for (size_t probe = 0; probe < slots_count_; ++probe) {
        Slot& s = slots_[(idx + probe) & (slots_count_ - 1)];
        uintptr_t empty = 0;
        if (s.from_addr.compare_exchange_strong(
                empty, key, std::memory_order_acq_rel)) {
            s.to_cap = to;
            installed_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // If another writer beat us with the same key, treat as a no-op:
        // ZGC's relocation invariants guarantee a single canonical forwarding
        // per from-address.
        if (s.from_addr.load(std::memory_order_acquire) == key) return;
    }
    // Table full. ZGC sizes regions to never exceed forwarding capacity;
    // hitting this is a bug.
    // TODO(phase1): assert + dump.
}

stopless_cap_t ForwardingTable::lookup(stopless_cap_t from) const {
    uintptr_t key = cap_addr(from);
    if (key == 0) return from;
    size_t idx = hash_addr(key, slots_count_);
    for (size_t probe = 0; probe < slots_count_; ++probe) {
        const Slot& s = slots_[(idx + probe) & (slots_count_ - 1)];
        uintptr_t f = s.from_addr.load(std::memory_order_acquire);
        if (f == 0)   return from;           // no forwarding installed
        if (f == key) return s.to_cap;       // hit
    }
    return from;                              // probe exhausted (shouldn't happen)
}

void ForwardingTable::clear_region(stopless_region_t* /*region*/) {
    // TODO(phase1): region-scoped clear (only touch slots whose to_cap
    // bounds intersect the region). Phase 0 implementation clears all;
    // correct but wasteful.
    for (size_t i = 0; i < slots_count_; ++i) {
        slots_[i].from_addr.store(0, std::memory_order_relaxed);
    }
    installed_.store(0, std::memory_order_relaxed);
}

}  // namespace stopless
