# Phase C-4 design — `StoplessAllocator` (bump-pointer + csetbounds)

**Status:** design
**Date:** 2026-05-27
**Phase:** C-4 (W3-W4)
**Depends on:** C-3 (StoplessArena wrapper exists)
**Acceptance:** A C-level standalone test (`test_alloc.c`) allocates
N objects via `stopless_alloc()`, verifies each cap has correct
bounds + permissions, exits 0. In-JVM acceptance comes with C-5.

## 1. Goal

Replace the nullptr-returning stubs (`StoplessArena::allocate` C++,
`stopless_alloc` C declared but not implemented) with a working
bump-pointer allocator that:

1. Atomically advances a per-arena bump offset (multi-mutator safe).
2. Returns a CHERI cap with **bounds tight to `size` bytes**
   (`cheri_bounds_set`).
3. Strips `CHERI_PERM_SW_VMEM` so the kernel revocation sweep
   subsequently invalidates this cap (Phase A/B already discovered
   this is required).
4. Handles OOM by returning nullptr (no GC trigger yet — that lands
   in C-9).

## 2. Why bump-pointer (and only bump-pointer for v1)

* **Simplicity** — no free list, no segregated size classes.
* **CHERI bounds precision** — for objects ≥ 4 KB on Morello, the
  cap base+top must be representable. `cheri_representable_length(N)`
  rounds N up to the smallest representable size; the base must be
  aligned to `~cheri_representable_alignment_mask(N)`. Bump-pointer
  trivially allows this by aligning the bump before the carve.
* **Concurrent move friendliness** — when GC moves objects, the
  "source" arena becomes read-only and a fresh "destination" arena
  bumps from offset 0. No fragmentation worry.

## 3. The CHERI alignment dance

```c
static inline size_t round_up_cheri(size_t n) {
    return cheri_representable_length(n);
}

static inline uintptr_t align_up_cheri(uintptr_t off, size_t n) {
    size_t mask = ~cheri_representable_alignment_mask(n); // bits that MUST be 0
    return (off + mask) & ~mask;
}
```

For 16 ≤ N ≤ 4 KB on Morello: mask is 0, alignment is byte-aligned.
For N = 8 KB: alignment is typically 16 B (one cap-word).
For N = 64 KB: alignment may be 256 B.
For N = 1 MiB: alignment is 16 KiB.

Java objects are mostly small (most under 256 B), so the precision
issue rarely bites. Large arrays (>=4 KB) do.

## 4. Concurrency

Use `_Atomic size_t bump_offset` with `compare_exchange_weak` in a
loop. Single-CAS fast path expected because contention only at
allocation-burst start time. If CAS contention becomes a bottleneck,
later phases can add TLAB chunks.

## 5. C API addition

Add a field to `stopless_arena_t`:

```c
typedef struct stopless_arena {
    void     *base;
    size_t    size;
    uint64_t *shadow;
    uintptr_t shadow_base_addr;
    _Atomic size_t bump_offset;    // NEW
} stopless_arena_t;
```

And implement (`allocator.c`):

```c
void *stopless_alloc(void *arena_, size_t size_bytes) {
    stopless_arena_t *a = (stopless_arena_t *)arena_;
    if (a == NULL || a->base == NULL || size_bytes == 0) return NULL;

    size_t rounded = cheri_representable_length(size_bytes);

    size_t old, new_off;
    do {
        old = atomic_load(&a->bump_offset);
        uintptr_t aligned_off = align_up_cheri((uintptr_t)old, rounded);
        new_off = aligned_off + rounded;
        if (new_off > a->size) return NULL;
        old = aligned_off - (aligned_off - old);  // restate for CAS
    } while (!atomic_compare_exchange_weak(&a->bump_offset, &old, new_off));

    char *obj = (char *)a->base + (new_off - rounded);
    obj = (char *)cheri_bounds_set(obj, size_bytes);
    obj = (char *)cheri_perms_and(obj, ~CHERI_PERM_SW_VMEM);
    return obj;
}
```

*(Note: the CAS loop needs care because we have to atomically jump
the aligned offset, not just bump. The actual implementation uses an
inner re-read; the snippet above is illustrative.)*

## 6. C++ wrapper update

`StoplessArena::allocate(bytes)` becomes:

```cpp
void* StoplessArena::allocate(size_t bytes) {
    if (_c == nullptr) return nullptr;
    void* p = stopless_alloc(_c, bytes);
    if (p == nullptr) return nullptr;
    // C-side already advanced its bump_offset; mirror into C++ used()
    Atomic::store(&_bump_offset, (size_t)stopless_arena_used(_c));
    return p;
}
```

We add `stopless_arena_used()` to api.h so the C++ side can report
`used()` without reading the C struct directly.

## 7. HotSpot integration

`StoplessHeap::mem_allocate` already calls `_arena->allocate(size *
HeapWordSize)`. After C-4, this returns a real cap. The caller
treats it as `HeapWord*`. Because purecap `HeapWord = void*` (16 B),
the pointer arithmetic in `mem_clear` and friends should work
without changes (patches 0060/0064 already aligned those).

## 8. TLAB (deferred)

OpenJDK's TLAB infrastructure pre-allocates a thread-local chunk of
heap memory for fast bump-pointer-without-CAS allocation. v1 of
StoplessGC does NOT use TLAB:

* `allocate_new_tlab(...)` is not overridden → returns the default
  zero-size TLAB, forcing every alloc to `mem_allocate`.
* Slower, but correct. Profile first; add TLAB only if
  contention is real.

## 9. OOM handling (deferred)

Returning nullptr from `mem_allocate` causes Java to throw
OutOfMemoryError. In C-9 we'll catch this in `mem_allocate` and
trigger a concurrent collection cycle before retrying. v1 of C-4
just throws OOM.

## 10. Tests

### 10.1 C-level: `tests/test_alloc.c`

```
1. arena_init(8 MiB)
2. Allocate 1000 objects of varying sizes (16 B, 64 B, 4 KB, 64 KB)
3. For each, assert:
   - cap tag is set
   - cap address ∈ [arena.base, arena.base + arena.size)
   - cap length >= requested size (>= because of cheri_representable_length)
   - cap perms have NO CHERI_PERM_SW_VMEM
4. Allocate until OOM, assert nullptr returned exactly when
   bump_offset + size > capacity.
```

### 10.2 C-level: `tests/test_alloc_concurrent.c`

```
1. arena_init(64 MiB)
2. Spawn 4 threads, each loops 10_000 times allocating 64 B
3. After join, assert:
   - Total allocations == 40_000
   - No two threads got overlapping caps (track set of base addrs)
   - bump_offset == 40_000 * round_up_cheri(64)
```

### 10.3 In-JVM (deferred to C-5)

`java -XX:+UseStoplessGC -Xms16m -Xmx16m -version` exits 0.

## 11. Patches

```
0084-cap-runtime-allocator.patch     — adds allocator.c + tests/test_alloc.c
0085-stopless-arena-allocate-wire.patch — C++ wrapper calls into stopless_alloc
```

## 12. Risks

1. **`cheri_representable_length` returns 0 for size 0** — guard
   already in place.
2. **CAS loop livelock** on extreme contention — unlikely with 4-8
   mutators; if it happens, exponential backoff.
3. **`align_up_cheri` for bump=0 case** — fine because aligning 0
   yields 0.
4. **Concurrent C++ `_bump_offset` mirror desync** — we just use
   the C-side as authority and copy on every allocate. Slightly
   stale `used()` for reporting is acceptable.
