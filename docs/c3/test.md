# Phase C-3 — test plan

**Status:** test plan, awaits run on hasee
**Date:** 2026-05-27

## Local pre-test (this machine, no JVM)

The C runtime that StoplessArena wraps was already tested in Phase
A/B:

```
cd src/cap_runtime/stopless_gc
make CROSS=1            # cross-build for purecap CheriBSD
# deploy + run on QEMU guest
./test_basic            # 1 obj move + handler fault — verified passing
./test_multi            # 32 obj + 2 threads — verified passing
                        # collector_moves=32 mutator_reads=259936 handler_faults=1
```

So C-3 inherits a known-good C foundation. The new work is the
HotSpot C++ binding.

## Stage 1 — patch apply

```bash
ssh bc@hasee
cd ~/projs/stopless-java-gc
./scripts/apply_patches.sh
```

Expected:
* `0082-stopless-runtime-link.patch` applies clean (Makefile only,
  one location in JvmFeatures.gmk)
* `0083-stopless-arena-cpp-bridge.patch` applies clean for the new
  files; the two hpp/cpp hunks of `stoplessHeap` may need rebase
  if hunks 0080 didn't apply cleanly.

If 0083 rejects: the cleanest fix is to re-edit stoplessHeap.hpp +
stoplessHeap.cpp by hand on hasee using the patch as a guide, then
regenerate via `git format-patch`.

## Stage 2 — build

```bash
# Build cap_runtime library first
cd ~/projs/stopless-java-gc/src/cap_runtime/stopless_gc
make CROSS=1            # produces libstopless_gc.a

# Now build hotspot
cd ~/projs/stopless-java-gc
./scripts/fast_iter.sh
```

Expected:
* `libstopless_gc.a` built (Phase A/B unchanged)
* hotspot links libjvm.so against libstopless_gc.a + libcheri_caprevoke
* No "undefined reference to `stopless_arena_init'" linker errors

Failure modes:
| Symptom | Fix |
|---|---|
| `STOPLESSGC enabled but ...libstopless_gc.a not built` warning | run `make` in cap_runtime first |
| `undefined reference to stopless_arena_init` | check JvmFeatures.gmk `JVM_LIBS_FEATURES` actually picked up |
| `incomplete type 'stopless_arena_t'` | the extern "C" include block in .cpp didn't apply |
| `cannot allocate object of abstract type` | pure virtual not overridden — usually missing `heap_committed_region` etc |

## Stage 3 — runtime behavior

```bash
./scripts/run.sh -XX:+UseStoplessGC -Xms32m -Xmx32m -version
```

Expected output (interleaved with -Xlog:gc):
```
openjdk version "17.0.x" ...
...
```

The interesting part is what happens BEHIND the scenes:
- `StoplessHeap::initialize()` is called
- `new StoplessArena(32 * 1024 * 1024)` runs
  - `stopless_init()` returns 0
  - `stopless_arena_init()` mmap's 32 MiB + acquires shadow bitmap
- BarrierSet installed
- JVM continues to `-version` short-circuit, prints version, exits 0

If the arena mmap fails (e.g. address-space pressure), VM aborts with
`StoplessArena: arena_init failed` — exit code != 0.

## Stage 4 — heap printout

```bash
./scripts/run.sh -XX:+UseStoplessGC -Xms64m -Xmx64m \
                 -Xlog:gc+heap=trace -version 2>&1 | grep -A 5 Stopless
```

Expected: roughly
```
Stopless Heap (CHERI-accelerated)
  StoplessArena: base=0x4xxxxxxxx capacity=65536K used=0K
Stopless barrier set: read=NOP (CHERI hw), write=NOP (Phase C-1)
```

## Stage 5 — is_in() sanity

A manual unit test inside the running JVM is awkward. Instead,
exercise via `-XX:+UnlockDiagnosticVMOptions -XX:+PrintHeapAtSIGBREAK`
and send SIGBREAK while the JVM idles, OR add a tiny `jtreg`-style
test in `src/cap_runtime/` that pokes the C library directly via the
non-JVM path (already covered by test_basic / test_multi).

## Stage 6 — regression on other GCs

```bash
./scripts/run.sh -XX:+UseEpsilonGC -version    # must still work
./scripts/run.sh -version                       # default GC
```

If either regresses, we broke something in shared (0081 likely
suspect).

## Sign-off

C-3 ships when:
- [ ] 0082 + 0083 apply on hasee (rebase as needed)
- [ ] libstopless_gc.a links into libjvm.so
- [ ] Stage 3: -version exits 0 with -XX:+UseStoplessGC
- [ ] Stage 4: -Xlog:gc+heap shows real `base=0x...` and `capacity=N K`
- [ ] Stage 6: other GCs still work

## Predicted blockers

1. **shift=64 sbfm/ubfm** (still C-6): -version may hit this during
   early VM init. Resolution: do C-6 in parallel or first.

2. **CHERI capability provenance during link**: libstopless_gc.a was
   built with the morello-sdk; libjvm.so is also built with the same
   sdk. Linking should be fine. If there's a "incompatible ABI"
   error, suspect the cap_runtime build vs hotspot build flags
   diverging (e.g., `-mabi=purecap` on one side, hybrid on the other).

3. **`SoftRefPolicy` default constructor**: if OpenJDK 17u changed
   this from no-arg to taking an arg, the initializer list
   `_soft_ref_policy()` will fail. Easy fix.
