# Phase C-3 — implementation notes

**Status:** patches drafted
**Patches:**
- `0082-stopless-runtime-link.patch` — Makefile glue
- `0083-stopless-arena-cpp-bridge.patch` — StoplessArena class + heap rewire
**Date:** 2026-05-27

## Decisions taken

### 1. Forward-declare `stopless_arena_t` in hpp, full include in cpp

`cap_runtime/.../revoke.h` includes C stdlib headers. We don't want
those leaking into HotSpot's precompiled-header path, where they
might collide with C++ wrapper macros. So:

```cpp
// stoplessArena.hpp
struct stopless_arena;
typedef struct stopless_arena stopless_arena_t;

// stoplessArena.cpp
extern "C" {
#include "revoke.h"
#include "api.h"
}
```

Cost: we store `stopless_arena_t*` (not value type) inside
`StoplessArena`. One extra heap alloc per arena. Trivial vs 64 MiB
mmap.

### 2. `vm_exit_during_initialization` on arena alloc failure

If `stopless_arena_init()` returns nonzero, we abort the VM rather
than returning an error code. Rationale: we're called from
`StoplessHeap::initialize()`, which is `CollectedHeap::initialize()`
in OpenJDK convention. By the time we're here, the JVM has committed
to using StoplessGC — there's nowhere to fall back to. Mirror
ZGC/G1 behaviour.

### 3. `capacity() / used() / max_capacity()` moved out-of-line

C-1 originally had these as inline `{ return _arena_capacity; }`.
The cpp-out conversion happens here because the body now reads
`_arena->capacity()`, which requires `StoplessArena` to be a complete
type. Forward-declaration in the header forbids the inline body, so
we move definitions to stoplessHeap.cpp.

### 4. `allocate()` still returns nullptr

C-3 is about wiring, not allocator logic. `StoplessArena::allocate()`
returns nullptr; `StoplessHeap::mem_allocate()` therefore also returns
nullptr — same observable behaviour as C-1. The difference is:
*everything else* now works — `is_in()`, `print_on()`, `capacity()`.
C-4 will swap in the bump-pointer body.

### 5. No `StoplessHeap::heap_committed_region()` override

OpenJDK's `CollectedHeap` has helpers like `reserved_region()` that
some callers (JFR, MXBeans) consult. C-3 does NOT override them
because the default implementation reports an empty region, which is
honest given that we don't expose a flat HeapWord-addressable space
in the conventional sense — the arena is a bag of cap-bounded
objects, not an addressable contiguous range as ZGC understands it.

Revisit if benchmarks need MXBean numbers.

### 6. Patches 0082 + 0083 are independent and can be split or merged

0082 is purely the build glue (Makefile). 0083 is pure C++ source.
Applying 0083 without 0082 produces a JVM that fails to link because
the StoplessArena ctor calls into `libstopless_gc.a` which isn't on
the link line. Applying 0082 without 0083 is harmless — it just adds
extra include paths and a library that nothing references.

In the series they apply 0082 first.

## Items deferred

| Item | To phase |
|---|---|
| `StoplessArena::allocate` actual bump-pointer logic | C-4 |
| NMT / MXBean integration | post C-12 |
| `heap_committed_region` override | post C-12 |
| Per-arena revoke (currently process-wide) | C-9 |
| Multi-arena support (currently 1 arena) | C-9 or later |

## Open questions (parking)

1. **Stack size of arena's bump pointer**: `volatile size_t
   _bump_offset` is currently locked by `Atomic::cmpxchg` in C-4. Do
   we need a per-thread TLAB to avoid contention? Profile first.

2. **Arena resize**: `MaxHeapSize` is fixed at init. ZGC supports
   shrinking when commitment exceeds usage. We don't. Acceptable for
   paper; revisit only if a benchmark fails OOM.

3. **HeapWord size mismatch**: under CHERI purecap, `HeapWordSize` is
   16 (patch 0060). `mem_allocate(size)` takes `size` in HeapWords,
   so the byte size is `size * 16`. The `_arena->allocate(size *
   HeapWordSize)` line in C-3 already does this, but C-4 needs to
   honor it.
