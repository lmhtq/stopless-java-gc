# Phase C-1 — implementation notes

**Status:** patch drafted
**Patch:** `patches/openjdk-jdk17/0080-stopless-gc-skeleton.patch`
**Date:** 2026-05-27

## Decisions taken

### 1. Modeled on Epsilon GC, not ZGC

Epsilon (`src/hotspot/share/gc/epsilon/`) is the smallest real GC in
OpenJDK 17 (~700 LoC). Its file layout is exactly what we need: heap +
arguments + barrier set + globals + nothing else. ZGC has 50+ files
including JIT codegen, which we don't need (CHERI hw replaces the
barrier).

### 2. CollectedHeap::Name extension deferred to C-2

Adding a new enum value (`CollectedHeap::Stopless`) is a build-system-
touching change because many files include `collectedHeap.hpp` and
some have `switch(kind())`. Bundling that with the new files in one
patch would make the diff harder to review. C-2 (the build wire-up
patch) is the natural home for it.

For C-1 to compile, the `kind()` override is commented `Name` from
the shared header — but actually, C-1 alone WON'T compile until C-2
adds the enum value. That's fine: C-1 + C-2 must be applied together
to test compile. The skeleton patch exists as a logical unit.

→ TODO: in C-2 patch add `enum Name { ..., Stopless }` to
  `collectedHeap.hpp`.

### 3. BarrierSet::Name extension also deferred

`stoplessBarrierSet.hpp` references `BarrierSet::StoplessBarrierSet`,
which needs to be added to the BarrierSet::Name enum in
`barrierSet.hpp`. Same story as above — bundled into C-2.

→ TODO: in C-2 patch add the BarrierSet::Name entry too.

### 4. `BarrierSetC2` reference

`stoplessBarrierSet.cpp` includes BarrierSetC2 unconditionally rather
than under `#ifdef COMPILER2`. Reason: OpenJDK 17 builds with C2 even
when we disable code paths, because some shared headers assume the
type exists. If the actual c2 directory is excluded (our case — clang
ICE forced us off C2), the type may still be linked as a stub. We can
revisit if linker errors show up.

### 5. `precompiled.hpp` inclusion order

OpenJDK convention: every `.cpp` must start with `#include
"precompiled.hpp"`. Forgetting this fails the build with a confusing
"no member named" error in shared headers. All 4 of our `.cpp` files
follow this.

### 6. Heap initial size logic in `initialize()`

```cpp
size_t min_byte_size = MAX2(MinHeapSize, (size_t)4 * M);
size_t max_byte_size = MAX2(MaxHeapSize, min_byte_size);
```

`MinHeapSize` defaults to 0 on a fresh JVM; we floor at 4MiB so the
arena size is sensible even when the user passes no `-Xms`. C-3 will
rewrite this when the real `StoplessArena::create()` lands.

### 7. `mem_allocate` returns nullptr

For `-XX:+UseStoplessGC -version`, no allocation actually happens
(class loading is delayed by `-version` short-circuiting). Returning
nullptr is enough to let the JVM hit and print the version banner. As
soon as we add a real test class with `new` operations, we'll hit
this nullptr and need C-4 ready.

## Files touched by THIS patch

```
src/hotspot/share/gc/stopless/stoplessHeap.hpp        +86 lines (new)
src/hotspot/share/gc/stopless/stoplessHeap.cpp        +71 lines (new)
src/hotspot/share/gc/stopless/stoplessArguments.hpp   +24 lines (new)
src/hotspot/share/gc/stopless/stoplessArguments.cpp   +26 lines (new)
src/hotspot/share/gc/stopless/stoplessBarrierSet.hpp  +49 lines (new)
src/hotspot/share/gc/stopless/stoplessBarrierSet.cpp  +29 lines (new)
src/hotspot/share/gc/stopless/stopless_globals.hpp    +25 lines (new)
src/hotspot/share/gc/stopless/vmStructs_stopless.hpp  +17 lines (new)
                                              total: ≈ 327 lines new code
```

## Items deferred

| Item | To phase |
|---|---|
| Enum extensions (`CollectedHeap::Name`, `BarrierSet::Name`) | C-2 |
| `gc_globals.hpp` include of stopless_globals.hpp | C-2 |
| `gcConfig.cpp` Stopless registration | C-2 |
| `JvmFeatures.gmk` + `jvm-features.m4` | C-2 |
| `-XX:+UseStoplessGC` parsing in `arguments.cpp` | C-2 |
| `StoplessArena` real arena mmap | C-3 |
| `mem_allocate` real bump-pointer | C-4 |

## Open questions (parking)

1. Does CHERI purecap C64 need a special `BarrierSet::AccessBarrier`
   instantiation for cap-typed oop fields? Investigate when first real
   read happens in C-5.

2. `mark_word` (lock-word slot) in cap-sized oop header — does our
   patch 0066 (`T_ADDRESS` injected fields) interact with the GC's
   header bits? Likely yes for moving GCs. Park for C-9.
