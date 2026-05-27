# Phase C-2 design — `-XX:+UseStoplessGC` build + flag wire-up

**Status:** design
**Date:** 2026-05-27
**Phase:** C-2 (W2)
**Depends on:** C-1 (0080-stopless-gc-skeleton.patch)
**Acceptance:** `java -XX:+UseStoplessGC -version` prints the JDK
version banner and exits 0, WITHOUT triggering any allocation that
would require C-4.

## 1. Goal

Extend OpenJDK's GC selection machinery so that:

1. `-XX:+UseStoplessGC` is a valid command-line flag.
2. When the user passes it, the JVM's `GCConfig` picks `StoplessArguments`,
   which in turn returns `new StoplessHeap()`.
3. The new `gc/stopless/*.cpp` files are compiled into libjvm.so.
4. The new `CollectedHeap::Stopless` and `BarrierSet::StoplessBarrierSet`
   enum values are recognized.

## 2. Files touched

### 2.1 Enum extensions (shared)

| File | Change |
|---|---|
| `src/hotspot/share/gc/shared/collectedHeap.hpp` | enum `Name` += `Stopless` |
| `src/hotspot/share/gc/shared/barrierSet.hpp` | enum `Name` += `StoplessBarrierSet` |

### 2.2 GC selection (shared)

| File | Change |
|---|---|
| `src/hotspot/share/gc/shared/gcConfig.cpp` | register Stopless in supported list |
| `src/hotspot/share/gc/shared/gcConfig.hpp` | declare `IS_STOPLESS_ONLY` macro |
| `src/hotspot/share/gc/shared/gc_globals.hpp` | `#include "gc/stopless/stopless_globals.hpp"` + threading macro |
| `src/hotspot/share/runtime/arguments.cpp` | accept `+UseStoplessGC` |
| `src/hotspot/share/runtime/flags/allFlags.hpp` | `GC_STOPLESS_FLAGS(...)` in mega-list |
| `src/hotspot/share/runtime/flags/jvmFlag.cpp` | (auto-picked up) |

### 2.3 JFR / serviceability (shared)

| File | Change |
|---|---|
| `src/hotspot/share/gc/shared/gcConfiguration.cpp` | report Stopless name to JFR |
| `src/hotspot/share/services/diagnosticCommand.cpp` | (no edit; will inherit) |

### 2.4 Build system

| File | Change |
|---|---|
| `make/hotspot/lib/JvmFeatures.gmk` | declare `stopless` feature, exclude/include logic |
| `make/autoconf/jvm-features.m4` | parse `--with-jvm-features=stopless` |
| `make/hotspot/symbols/symbols-aix` | (skip — we don't target AIX) |

## 3. Flag definition

In `stopless_globals.hpp` we DECLARE the flag struct. To make
`UseStoplessGC` a real flag in `globals.hpp`, C-2 adds:

```cpp
// gc_globals.hpp insertion:
#include "gc/stopless/stopless_globals.hpp"
// ...

#define GC_FLAGS(develop, ...)               \
   GC_G1_FLAGS(develop, ...)                 \
   GC_PARALLEL_FLAGS(develop, ...)           \
   GC_SERIAL_FLAGS(develop, ...)             \
   GC_SHENANDOAH_FLAGS(develop, ...)         \
   GC_EPSILON_FLAGS(develop, ...)            \
   GC_Z_FLAGS(develop, ...)                  \
   GC_STOPLESS_FLAGS(develop, ...)           \    // NEW
   product(bool, UseStoplessGC, false,             \    // NEW
           "Use the Stopless garbage collector")   \    // NEW
```

(Note: `UseStoplessGC` lives at the top of `GC_FLAGS` because that's
where `UseZGC`, `UseG1GC` etc live. Putting it inside
`GC_STOPLESS_FLAGS` would also work but mismatches the convention.)

## 4. GCConfig registration

```cpp
// gcConfig.cpp, in the table:
static const SupportedGC SupportedGCs[] = {
   SupportedGC(UseSerialGC,         CollectedHeap::Serial,        ...),
   SupportedGC(UseParallelGC,       CollectedHeap::Parallel,      ...),
   SupportedGC(UseG1GC,             CollectedHeap::G1,            ...),
   SupportedGC(UseConcMarkSweepGC,  CollectedHeap::Serial,        ...),
   SupportedGC(UseEpsilonGC,        CollectedHeap::Epsilon,       ...),
   SupportedGC(UseShenandoahGC,     CollectedHeap::Shenandoah,    ...),
   SupportedGC(UseZGC,              CollectedHeap::Z,             ...),
   SupportedGC(UseStoplessGC,       CollectedHeap::Stopless,
               stoplessgc::name, "stopless gc"),     // NEW
};
```

`stoplessgc::name` is `StoplessArguments`. We add a free factory
function or just instantiate in the cpp.

## 5. Build feature

Pragmatically the simplest path: piggyback Stopless onto Epsilon's
build feature (since they're structurally similar). Then `make
hotspot` includes us unconditionally.

```makefile
# JvmFeatures.gmk
ifneq ($(call check-jvm-feature, stopless), true)
  JVM_CFLAGS_FEATURES += -DINCLUDE_STOPLESSGC=0
  JVM_EXCLUDE_PATTERNS += gc/stopless
endif
```

Default `--with-jvm-features` includes everything; the
exclude-pattern only fires if the user OPTS OUT. For us this means we
don't need to pass any extra `--with-...` flag to `configure`.

## 6. Verification (this phase)

```bash
# Build (cross from hasee)
./scripts/apply_patches.sh
./scripts/fast_iter.sh

# Test 1: flag exists
./scripts/run.sh -XX:+PrintFlagsFinal -version 2>&1 | grep -i stopless
# Expected:
#   bool UseStoplessGC                                                = false       {product} {default}
#   size_t StoplessArenaSize                                          = 268435456    {product} {default}

# Test 2: enable, run -version
./scripts/run.sh -XX:+UseStoplessGC -version
# Expected: prints openjdk version, exits 0

# Test 3: log GC init
./scripts/run.sh -XX:+UseStoplessGC -Xlog:gc -version 2>&1 | grep -i stopless
# Expected: "Using Stopless" or "Stopless Heap" log line

# Test 4: confirm StoplessHeap is the real heap
./scripts/run.sh -XX:+UseStoplessGC -XX:+PrintHeapAtSIGBREAK ...
# (or just confirm via -Xlog:gc+heap=info)
```

## 7. Risks

* **`-Werror=switch` in shared GC code**: adding a `Name::Stopless`
  enum value will fail-build any switch that doesn't have a default.
  Strategy: grep the tree for `switch.*kind()` and add no-op cases.
* **Epsilon as fallback for unsupported ops**: some shared GC code
  asserts `kind() != Epsilon && kind() != Stopless` or similar. Watch
  for these.
* **JFR event registration**: gc kind name strings go into JFR
  serialization. Easy to add but easy to forget.

## 8. Patch list (this phase)

```
0081-stopless-gc-feature-enable.patch
```

Single patch; contains all the build wire-up and enum extensions.
