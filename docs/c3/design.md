# Phase C-3 design — `StoplessArena` C++ wrapper

**Status:** design
**Date:** 2026-05-27
**Phase:** C-3 (W3)
**Depends on:** C-1 (skeleton compiles), cap_runtime/stopless_gc (Phase A/B)
**Acceptance:** `StoplessHeap::initialize()` allocates a real arena
via cap_runtime, and `is_in(p)` returns true for pointers in the arena.

## 1. Goal

Bridge the C `cap_runtime/stopless_gc/api.h` into HotSpot's C++ class
hierarchy. Specifically:

* Provide `StoplessArena` (C++ class) that wraps `stopless_arena_t`
  (C struct from `cap_runtime/.../revoke.h`).
* Lifetime is RAII: constructor calls `stopless_arena_init`, destructor
  calls `stopless_arena_fini`.
* Expose `contains(addr)`, `base()`, `size()`, `used()`, `alloc(size)`,
  `mark_for_revoke(cap)`.
* Replace the placeholder `_arena_capacity = MaxHeapSize` logic in
  `StoplessHeap::initialize()` with a real `_arena = new StoplessArena(...)`
  call.

## 2. Why a C++ wrapper rather than direct C calls

HotSpot is a C++ codebase using:
* `mtGC` memory tracking type
* `CHeapObj` RAII allocation
* `assert(...)` from `globalDefinitions.hpp`
* `os::commit_memory` and related VM-aware helpers

A thin C++ class lets us:
* Hold C++ destructors that auto-clean (no manual goto out: lifetimes)
* Pass via reference rather than raw `stopless_arena_t*`
* Plug into HotSpot's memory tracking (`MemoryService`, `JFR`)
* Add `verify()` and `print_on()` that match HotSpot's conventions

## 3. Class outline

```cpp
class StoplessArena : public CHeapObj<mtGC> {
 private:
  stopless_arena_t _c_arena;       // from cap_runtime/.../revoke.h
  size_t           _capacity;
  volatile size_t  _bump_offset;   // C-4 will use this for alloc

 public:
  // RAII: init the C arena (mmap + shadow alloc) on construction.
  // Aborts via vm_exit() if the underlying allocation fails.
  StoplessArena(size_t capacity_bytes);
  ~StoplessArena();

  // Coordinate queries
  void* base() const;              // C pointer, cap-typed under CHERI
  size_t capacity() const          { return _capacity; }
  size_t used() const              { return _bump_offset; }
  bool contains(const void* p) const;

  // Allocation (C-4 will refine, here just stub)
  void* allocate(size_t size);

  // Mover-side ops (C-9 will use these, here just forward to C)
  int mark_for_revoke(void* cap);
  int revoke_sweep();

  // HotSpot diagnostics
  void verify();
  void print_on(outputStream* st) const;

 private:
  // non-copyable
  StoplessArena(const StoplessArena&) = delete;
  StoplessArena& operator=(const StoplessArena&) = delete;
};
```

## 4. Where it lives

Two implementation options:

**Option A: `src/hotspot/share/gc/stopless/stoplessArena.{hpp,cpp}`**
Inside the GC directory. Build-system already picks up `*.cpp` there.
Linking: `stoplessHeap.cpp` calls into our C library
(`libstopless_gc.a` from `cap_runtime/stopless_gc/`) via `extern "C"`
includes of `cap_runtime/stopless_gc/api.h`.

**Option B: `src/hotspot/share/gc/stopless/` includes the C source
files directly via symlink or build glue.**

**Decision: Option A.** It keeps the C runtime as an independent
library (we still want to run the standalone tests there!), and the
JVM just links against it. Patch 0071 (placeholder in the series)
hooks `libstopless_gc.a` into libjvm.so's link line. C-3 picks up
that hook formally.

## 5. Coordination with cap_runtime

`cap_runtime/stopless_gc/` already exports:

```c
int  stopless_init(void);
void stopless_shutdown(void);
int  stopless_arena_init(stopless_arena_t *out, size_t size);
void stopless_arena_fini(stopless_arena_t *a);
int  stopless_mark_revoke_cap(stopless_arena_t *a, void *obj_cap);
int  stopless_revoke_now(void);
```

C-3 wraps each of these in the C++ class. No new C API additions
needed.

`stopless_alloc()` does not exist yet — the C library is mover/
revoke-focused. C-4 will add it; C-3's `allocate()` is a stub that
returns nullptr.

## 6. Build wiring

`make/hotspot/lib/JvmFeatures.gmk` extension (now that 0081 is in):

```makefile
ifneq ($(call check-jvm-feature, stoplessgc), true)
  ...
else
  # Link libstopless_gc.a from cap_runtime/
  JVM_LIBS_FEATURES += $(STOPLESS_RUNTIME_LIBDIR)/libstopless_gc.a
  JVM_LIBS_FEATURES += -lcheri_caprevoke
  JVM_CFLAGS_FEATURES += -DINCLUDE_STOPLESSGC=1
  JVM_CFLAGS_FEATURES += -I$(STOPLESS_RUNTIME_INCDIR)
endif
```

where `$(STOPLESS_RUNTIME_LIBDIR)` etc are set by configure to
`$(TOPDIR)/../cap_runtime/stopless_gc/`. Patch 0082 will add this.

## 7. Verification

Local (without JVM): Already verified — `test_basic` and `test_multi`
in `cap_runtime/stopless_gc/tests/` confirm the C side works.

In-JVM (after C-3 ships paired with 0080+0081+0082):

```bash
./scripts/run.sh -XX:+UseStoplessGC -Xms64m -Xmx64m -version
```

Expected: JVM starts. `StoplessHeap::initialize()` calls `new
StoplessArena(64*M)` which mmap's 64 MiB and registers it with cap
revocation. `print_on(tty)` (triggered by `-XX:+PrintHeapAtSIGBREAK`
or `-Xlog:gc+heap`) reports:

```
Stopless Heap (CHERI-accelerated)
  arena: base=0x..., capacity=64M, used=0
  forwarding-table: 0 entries
```

## 8. Risks

1. **Header collision**: `cap_runtime/.../api.h` includes `<stdint.h>`
   etc. HotSpot's `precompiled.hpp` may have already pulled in
   incompatible headers (e.g. C++ wrappers around C stdlib). Wrap the
   include in `extern "C" { ... }` to be safe.
2. **`stopless_arena_t` struct visibility**: it's defined in
   `revoke.h`. Including that in a HotSpot `.hpp` exposes the C
   types — fine, but we should keep it out of pre-compiled headers
   (use forward-declaration in `stoplessArena.hpp`, full include only
   in `stoplessArena.cpp`).
3. **NMT (Native Memory Tracking)** doesn't see our mmap'd arena.
   For paper purposes, not load-bearing. Can revisit later.

## 9. Patch list (this phase)

```
0082-stopless-runtime-link.patch          — Makefile + configure glue
0083-stopless-arena-cpp-bridge.patch      — StoplessArena class + initialize()
```
