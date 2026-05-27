# Phase C-2 — implementation notes

**Status:** patch drafted, untested on hasee
**Patch:** `patches/openjdk-jdk17/0081-stopless-gc-feature-enable.patch`
**Date:** 2026-05-27

## Decisions taken

### 1. Line-number drift caveat

This patch was authored without access to the openjdk-jdk17u tree
(which lives on hasee at `~/projs/stopless-java-gc/third_party/`).
Line numbers in hunk headers are best-effort based on the OpenJDK 17u
GA layout. Expected risks:

* `collectedHeap.hpp` line 150 — likely close, may be ±10
* `gc_globals.hpp` line 36 / 82 / 126 — these are insertion points
  in a generated-looking macro list; context is the surrounding
  GC names which are stable across 17u updates
* `arguments.cpp` line 2034 — the `parse_xss` neighborhood has been
  refactored between 17u releases, may need rebase
* `macros.hpp` line 158 — `EPSILONGC_ONLY` macro definition area;
  stable
* `JvmFeatures.gmk` line 127 — Epsilon feature block; stable

**Rebase strategy if hunks reject:** apply the rejected hunks
manually on hasee, then `cd third_party/openjdk-jdk17u && git diff
src/hotspot/share/gc/shared/ ... > /tmp/regen.patch`, copy
`/tmp/regen.patch` back into `patches/openjdk-jdk17/0081-...`.

### 2. `INCLUDE_STOPLESSGC` defaults to 1

We default the macro to ON (`INCLUDE_STOPLESSGC=1`) so the new GC
compiles into every build by default — same convention as Epsilon.
Users who want a slimmer JVM can pass
`--with-jvm-features=-stoplessgc` to `configure`.

### 3. Feature name lowercase `stoplessgc`

JVM features are conventionally lowercase, no hyphens. Following
`epsilongc`, `parallelgc`, etc.

### 4. SupportedGC table order

Placed between Epsilon and Z in the `SupportedGCs[]` table. This
mirrors the alphabetical-ish ordering and keeps the diff small.

### 5. No JFR registration patch (deferred)

OpenJDK's `gcConfiguration.cpp` enumerates GCs for JFR. C-2 skips
this — JFR will report Stopless as "Unknown" GC initially. JFR
visibility is non-blocking for the paper (we won't profile via JFR).
Fix later if needed.

### 6. ALL_GCS_DO macro

`gcConfig.hpp` has an `ALL_GCS_DO(...)` x-macro for iterating GCs.
This patch does NOT extend it because we don't currently call any of
the gen-via-macro APIs from outside Stopless. If something breaks
that needs all-GCs introspection, add `ALL_GCS_DO` entry then.

## Files touched

```
src/hotspot/share/gc/shared/collectedHeap.hpp     +1 line
src/hotspot/share/gc/shared/barrierSet.hpp        +1 line
src/hotspot/share/gc/shared/gc_globals.hpp        +12 lines
src/hotspot/share/gc/shared/gcConfig.cpp          +5 lines
src/hotspot/share/gc/shared/gcConfig.hpp          (no edit, included for context)
src/hotspot/share/runtime/arguments.cpp           +3 lines
src/hotspot/share/utilities/macros.hpp            +11 lines
make/hotspot/lib/JvmFeatures.gmk                  +7 lines
make/autoconf/jvm-features.m4                     +1 word
                                                  total: ≈ 41 lines new
```

## Items deferred

| Item | To phase |
|---|---|
| JFR GC name registration | post-C-12 polish |
| `gcConfiguration.cpp` serialization | post-C-12 polish |
| `ALL_GCS_DO` extension | only if needed |
| `serviceability/sa/` for SA agent | not required for paper |

## Known issues to surface during build

1. **`-Werror=switch` on `CollectedHeap::Name`**: at minimum these
   files switch on it and may need default-cases added:
   - `gc/shared/gcConfig.cpp` (the table)
   - `gc/shared/genCollectedHeap.cpp` (may not be relevant)
   - JFR code under `jfr/leakprofiler/`
   Plan: grep `switch.*kind()` after first build attempt, add cases.

2. **`-Werror=switch` on `BarrierSet::Name`**: typical sites:
   - `gc/shared/barrierSetConfig.hpp` x-macro
   - Various `assert(... == BarrierSet::ZBarrierSet, ...)`
   Plan: similar grep.

3. **`BarrierSetConfig` registration**: `barrierSetConfig.hpp` has
   an `FOR_EACH_CONCRETE_BARRIER_SET_DO` macro that lists all
   concrete barrier sets. We may need an entry like:
   ```
   f(StoplessBarrierSet)
   ```
   Discovered if linker fails for vtable.

4. **`barrierSetAssembler_aarch64.cpp` instantiation**: each
   BarrierSet has an aarch64 assembler. We use the default
   (no-op) one, so should work without an explicit
   `StoplessBarrierSetAssembler`. Verify at first build.

## Open questions (parking)

1. Should `StoplessBarrierSet` inherit `ModRefBarrierSet` (G1 / Card
   style) or `BarrierSet` directly? For v1 it's BarrierSet directly
   since we have no remembered set. May change in C-10.

2. Does `-XX:+UseStoplessGC` need a sanity-check against `-XX:+UseG1GC`
   (mutually exclusive)? OpenJDK's argument parser already enforces
   "only one GC selected" via the `SupportedGCs` table. Should work
   out of the box.
