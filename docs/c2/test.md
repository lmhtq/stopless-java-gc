# Phase C-2 — test plan

**Status:** test plan, awaits run on hasee
**Date:** 2026-05-27

## Test environment

* Build host: hasee (bc@hasee)
* Cross-build: morello-sdk in `~/projs/stopless-java-gc/third_party/output/morello-sdk`
* Sysroot: `~/projs/stopless-java-gc/third_party/output/rootfs-morello-purecap`
* Test target: CheriBSD QEMU guest

## Stage 0 — patch apply

```bash
ssh bc@hasee
cd ~/projs/stopless-java-gc
./scripts/apply_patches.sh
```

Expected:
* `0080-stopless-gc-skeleton.patch` applies clean (new files only)
* `0081-stopless-gc-feature-enable.patch` applies, possibly with
  `.rej` files for some hunks. If rejects:
  * Manually apply the intent (typically 2-3 lines added per hunk)
  * Regenerate: `cd third_party/openjdk-jdk17u && git diff > /tmp/0081-regen.patch`
  * Copy back to `patches/openjdk-jdk17/0081-stopless-gc-feature-enable.patch`

## Stage 1 — build

```bash
./scripts/fast_iter.sh
```

Expected:
* hotspot compiles the new `src/hotspot/share/gc/stopless/*.cpp` (7 files)
* No `-Werror=switch` failures (if present, see impl_notes §3)
* No linker errors for `StoplessHeap`, `StoplessBarrierSet`,
  `StoplessArguments` vtables
* `libjvm.so` deploys to QEMU guest

Failure-mode actions:

| Symptom | Action |
|---|---|
| `error: 'Stopless' not in enum ... Name` | check 0081 collectedHeap.hpp hunk applied |
| `undefined reference to vtable for StoplessBarrierSet` | add `FOR_EACH_CONCRETE_BARRIER_SET_DO(StoplessBarrierSet)` |
| `unhandled case Stopless` | grep `switch.*kind()` and add default |
| `-DINCLUDE_STOPLESSGC=0` in JVM_CFLAGS | feature exclude triggered; fix JvmFeatures.gmk |

## Stage 2 — flag visibility

```bash
./scripts/run.sh -XX:+PrintFlagsFinal -version 2>&1 | grep -i stopless
```

Expected output (3 lines):
```
   size_t StoplessArenaSize     = 268435456                {product} {default}
   uintx  StoplessGCCount       = 0                        {diagnostic} {default}
   bool   UseStoplessGC         = false                    {product} {default}
```

## Stage 3 — select GC

```bash
./scripts/run.sh -XX:+UseStoplessGC -version
```

Expected:
* JVM prints `openjdk version "17..." ...`
* Exits 0
* No warning about GC selection

Note: `-version` does NOT trigger object allocation, so the
nullptr-returning `mem_allocate` in C-1 skeleton is never hit. This
is the upper bound on what C-2 alone can demonstrate.

## Stage 4 — log GC init

```bash
./scripts/run.sh -XX:+UseStoplessGC -Xlog:gc -version 2>&1 | head -10
```

Expected: at least one log line such as
```
[0.001s][info][gc] Using Stopless
```

## Stage 5 — combined sanity

```bash
# Sanity: epsilon still works (we didn't break other GCs)
./scripts/run.sh -XX:+UseEpsilonGC -version
# Expected: prints version, exits 0

# Sanity: serial still works
./scripts/run.sh -version
# Expected: prints version (default GC selected), exits 0
```

## Sign-off criteria

C-2 ships when:
- [ ] 0081 patch applies (possibly after rebase) on hasee
- [ ] hotspot builds with the new GC compiled in
- [ ] Stage 2: 3 flag lines visible
- [ ] Stage 3: `-XX:+UseStoplessGC -version` exits 0
- [ ] Stage 4: at least one Stopless-related gc-log line
- [ ] Stage 5: other GCs still work (regression check)
- [ ] Update `docs/c2/test.md` with actual results (any deviations
      from expected, exact line numbers found, etc)

## Predicted blockers and pre-emptive fixes

1. **shift=64 sbfm/ubfm** (C-6): even `-version` may trip this if
   it happens during early VM init before the GC is consulted.
   If so, prioritize C-6 then return to C-2 acceptance.

2. **purecap CHERI bsd build**: 16-byte HeapWord (patch 0060) plus
   StoplessHeap's `_arena_capacity = MaxHeapSize` — for `-Xms0
   -Xmx0` defaults this is 0 which may trip an assertion. Fix:
   floor to 4 MiB (already in C-1 `initialize()`).
