# Phase C-1 design — StoplessGC HotSpot skeleton

**Status:** design
**Date:** 2026-05-27
**Phase:** C-1 (W1-W2)
**Acceptance:** hotspot compiles with `--with-jvm-features=stopless` (or equivalent), no behaviour required.

## 1. Goal of this phase

Add the minimum scaffolding for a new GC inside `src/hotspot/share/gc/stopless/` so that:

1. `make hotspot` succeeds with the new files included.
2. `gc_globals.hpp` exposes `UseStoplessGC` (boolean flag).
3. The `CollectedHeapCreator` / `GCArguments` machinery can be later wired to instantiate `StoplessHeap` (full wire-up is C-2; C-1 only adds the class).
4. All virtual methods are stubbed with `ShouldNotReachHere()` or sensible no-op; we are NOT trying to run anything yet.

## 2. File inventory (new files)

Modeled on `src/hotspot/share/gc/epsilon/` (which is the smallest real GC in the tree and matches our minimal-viable target).

```
src/hotspot/share/gc/stopless/
├── stoplessArguments.cpp
├── stoplessArguments.hpp        — : GCArguments
├── stoplessBarrierSet.cpp
├── stoplessBarrierSet.hpp       — : BarrierSet (read = NOP, store = NOP for v1)
├── stoplessHeap.cpp
├── stoplessHeap.hpp             — : CollectedHeap
├── stopless_globals.hpp         — flag UseStoplessGC declared here
└── vmStructs_stopless.hpp       — empty for v1
```

## 3. Class skeletons

### 3.1 `StoplessHeap : public CollectedHeap`

```cpp
class StoplessHeap : public CollectedHeap {
 public:
  StoplessHeap();

  // CollectedHeap pure-virtuals — minimum stubs
  Name kind() const override                              { return CollectedHeap::Stopless; }
  const char* name() const override                       { return "Stopless"; }
  jint initialize() override;                             // mmap heap reservation
  size_t capacity() const override                        { return _arena_capacity; }
  size_t used() const override                            { return _arena_used; }
  bool is_in(const void* p) const override                { return _arena.contains(p); }
  HeapWord* mem_allocate(size_t size, bool* gc_overhead_limit_was_exceeded) override;
  void collect(GCCause::Cause cause) override             { /* C-9 will fill in */ }
  void do_full_collection(bool clear_all_soft_refs) override { /* no-op v1 */ }
  void object_iterate(ObjectClosure* cl) override         { ShouldNotReachHere(); }
  void print_on(outputStream* st) const override;
  jlong millis_since_last_gc() override                   { return 0; }
  void prepare_for_verify() override                      { /* no-op */ }
  void verify(VerifyOption option) override               { /* no-op */ }

 private:
  size_t _arena_capacity;
  size_t _arena_used;
  // StoplessArena _arena;   // filled by C-3
};
```

Plus `CollectedHeap::Name` enum gains a `Stopless` member in `shared/collectedHeap.hpp` (one-line patch).

### 3.2 `StoplessArguments : public GCArguments`

```cpp
class StoplessArguments : public GCArguments {
 public:
  void initialize() override;
  size_t conservative_max_heap_alignment() override { return UseLargePages ? os::large_page_size() : os::vm_page_size(); }
  CollectedHeap* create_heap() override { return new StoplessHeap(); }
};
```

### 3.3 `StoplessBarrierSet : public BarrierSet`

For C-1 we inherit `BarrierSet::AccessBarrier` as **no-op** — every oop load/store
goes straight through. The hardware cap-tag check is the "barrier" and lives at the
ISA level; it doesn't need software emission. C-10 may revisit for the write path.

```cpp
class StoplessBarrierSet : public BarrierSet {
 public:
  StoplessBarrierSet();
  void on_thread_create(Thread* thread) override        { /* no-op */ }
  void on_thread_destroy(Thread* thread) override       { /* no-op */ }
  void on_thread_attach(Thread* thread) override        { /* no-op */ }
  void on_thread_detach(Thread* thread) override        { /* no-op */ }
  void print_on(outputStream* st) const override;

  template <DecoratorSet decorators, typename BarrierSetT = StoplessBarrierSet>
  class AccessBarrier: public BarrierSet::AccessBarrier<decorators, BarrierSetT> {};
};
```

### 3.4 `stopless_globals.hpp`

```cpp
#define GC_STOPLESS_FLAGS(develop,                                         \
                          develop_pd, product, product_pd,                 \
                          notproduct, range, constraint)                   \
  product(bool, UseStoplessGC, false,                                      \
          "Use the Stopless garbage collector")                            \
                                                                            \
  product(size_t, StoplessMinHeapSize, 0,                                  \
          "Initial size of Stopless heap")                                 \
                                                                            \
  product(size_t, StoplessMaxHeapSize, 0,                                  \
          "Maximum size of Stopless heap")
```

## 4. Files touched (existing tree)

| File | Edit |
|---|---|
| `src/hotspot/share/gc/shared/collectedHeap.hpp` | add `Stopless` to enum `Name` |
| `src/hotspot/share/gc/shared/gc_globals.hpp` | `#include "gc/stopless/stopless_globals.hpp"` + threading flag |
| `src/hotspot/share/runtime/arguments.cpp` | parse `-XX:+UseStoplessGC` → select Stopless |
| `src/hotspot/share/gc/shared/gcConfig.cpp` | register Stopless in the GC table |
| `src/hotspot/share/gc/shared/gcConfiguration.cpp` | metadata for JFR |
| `make/hotspot/lib/JvmFeatures.gmk` | add `stopless` feature |
| `make/autoconf/jvm-features.m4` | declare `stopless` feature |

## 5. Patch layout

Single patch: `0080-stopless-gc-skeleton.patch`.

C-2 will add the build-system wiring (`JvmFeatures.gmk`, `jvm-features.m4`) as `0081-stopless-gc-feature-enable.patch`.

## 6. Verification (this phase)

```
$ ./scripts/apply_patches.sh
$ ./scripts/fast_iter.sh    # cross-build on hasee
$ ./scripts/deploy.sh && ./scripts/run.sh -XX:+PrintFlagsFinal -version | grep UseStoplessGC
```

Expected: the flag prints as `bool UseStoplessGC = false {product}`. We do NOT
turn it on in C-1 — that comes in C-2.

## 7. Risks for this phase

* `CollectedHeap::Name` is an enum used in many switch statements. Adding `Stopless`
  triggers `-Werror=switch` failures in places that switch on `kind()`. Mitigation:
  add `default: break;` clauses or explicit `case Stopless:` arms.
* Some shared GC code includes ALL per-GC headers via `gc_globals.hpp` chain.
  Watch for circular includes between `stopless_globals.hpp` and `BarrierSet`.

## 8. Cross-references

* Phase C overview: `docs/17_phase_c_overview.md`
* Epsilon GC structure (reference template): `third_party/openjdk-jdk17u/src/hotspot/share/gc/epsilon/`
