# Phase C-4 — implementation notes

**Status:** patches drafted
**Files added/changed:**
- `src/cap_runtime/stopless_gc/allocator.h` (new)
- `src/cap_runtime/stopless_gc/allocator.c` (new)
- `src/cap_runtime/stopless_gc/revoke.h` (added `bump_offset` field)
- `src/cap_runtime/stopless_gc/Makefile` (added allocator.o + 2 tests)
- `src/cap_runtime/stopless_gc/tests/test_alloc.c` (new)
- `src/cap_runtime/stopless_gc/tests/test_alloc_concurrent.c` (new)
- `patches/openjdk-jdk17/0085-stopless-arena-allocate-wire.patch` (new)
**Date:** 2026-05-27

## Decisions taken

### 1. CAS loop on `_Atomic size_t bump_offset`

Standard lock-free bump-pointer. Each iteration:
1. Read current offset (relaxed).
2. Round UP to CHERI representable alignment for the requested size.
3. Compute new offset = aligned + rounded.
4. CAS-publish; if it fails, the inner loop re-reads `cur` (which the
   CAS already updated for us) and retries.

ABA is not an issue because the bump offset is monotonically
increasing within an allocation cycle. Reset to 0 (after revoke
sweep) happens single-threaded.

### 2. Rounded size vs requested size

We RESERVE `cheri_representable_length(N)` bytes (potentially > N),
but the returned cap has bounds set to N (`cheri_bounds_set(obj, N)`).

This wastes a few bytes per allocation when N is large (and CHERI
precision forces alignment). The win: the cap is exactly N bytes
wide as far as the user is concerned, which simplifies GC reasoning.

If profiling shows this waste matters, we can revisit. v1 keeps it
simple.

### 3. C++ side caches `_bump_offset`

`StoplessArena::allocate()` mirrors the C-side bump into a C++ field
after each call. `StoplessArena::used()` then reads the C-side atomic
authoritatively (cheap on aarch64 — single load with acquire).

The mirror is a leftover from the original C-3 design. Could be
deleted; left in case future code wants to read it without the C++
→ C function call cost. Negligible either way.

### 4. C/C++ atomic compatibility

`_Atomic size_t` is C11. C++ doesn't grok it (even inside
`extern "C"` blocks the field type must be parseable as C++). Fix:

```c
#ifdef __cplusplus
#  define STOPLESS_ATOMIC_SIZE_T size_t
#else
#  include <stdatomic.h>
#  define STOPLESS_ATOMIC_SIZE_T _Atomic size_t
#endif
```

On aarch64 the two have identical size and alignment (8 bytes, 8-byte
aligned). The C++ side never reads `bump_offset` directly — it always
goes through `stopless_arena_used()`/`stopless_alloc()`, both
implemented in C. So C++'s view of the field is layout-only.

### 5. OOM policy

`stopless_alloc()` returns NULL on OOM. `StoplessHeap::mem_allocate`
logs a warning and propagates NULL → Java throws OOM.

Real GC behaviour (C-9): on alloc failure, trigger a concurrent
collection cycle, wait for it to free space, retry once, then OOM.
Out of scope for C-4.

### 6. No TLAB

`CollectedHeap::allocate_new_tlab` is not overridden. The default
implementation returns a zero-size TLAB, forcing every Java
`new` operation through the slow path (one CAS into our bump
allocator).

CAS on a single global is fine for v1 — Java allocation rates are
< 100M obj/sec per thread in practice, and a CAS is ~5 ns even
contended. Profile under DaCapo (C-12) before adding TLAB.

### 7. `cheri_representable_length` rounding overflow

If `N` is close to `SIZE_MAX`, rounding up could overflow. We
defensively check `if (rounded < size_bytes) return NULL`. In
practice Java cap allocations are bounded by `MaxHeapSize` so this
never trips, but it's free insurance.

## Items deferred

| Item | To phase |
|---|---|
| OOM → trigger GC → retry | C-9 |
| TLAB | post-C-12 only if profiling demands |
| Per-thread alloc cursor | post-C-12 |
| Allocation logging at trace level | post-C-12 |
| Pre-zeroing the new object | NOT needed — mmap'd arena is already 0-init |

## Open questions

1. **Should `allocate` be inlineable for hot path?** HotSpot's
   `CollectedHeap::mem_allocate` is virtual; can't inline. JIT'd
   allocation paths bypass the virtual via inlined fast path code.
   ZGC/G1 have an `allocate_with_tlab` fast path. We don't, yet.

2. **Should `stopless_alloc` accept `__uintcap_t arena` instead of
   `void *arena`?** Practically `void *` works because clang's
   purecap mode treats them equivalently. Keeping `void *` for
   readability.

3. **Cap-tag preservation in `memset`?** When we `memset(obj, 0xAB, n)`
   in tests, this writes bytes; it does NOT zero the cap-tags of
   stored caps. For Java the runtime uses `Copy::*` which preserves
   tags on cap-typed words. We already have patches 0014/0017/0020/
   0064 for that.
