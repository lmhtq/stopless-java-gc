# Phase C-4 — test plan

**Status:** test plan; C-level tests can run on hasee TODAY without
waiting for the JVM-side patches to apply
**Date:** 2026-05-27

## Stage A: C-level standalone tests (highest priority)

These exercise the bump allocator in pure C, no JVM needed. Runnable
on hasee/QEMU as soon as the cap_runtime is rebuilt.

### A.1 — `test_alloc`

```bash
ssh bc@hasee
cd ~/projs/stopless-java-gc/src/cap_runtime/stopless_gc
make CROSS=1 clean
make CROSS=1 test_alloc
scp test_alloc cheri-qemu:/root/
ssh cheri-qemu ./test_alloc
```

Expected output:
```
test_alloc: init
  varying-size: used=N/8388608 after 7 allocs
  no-overlap: PASS
  read-write: PASS
  OOM after K allocs of 1024 (arena=64K)
  reset: PASS
OK  test_alloc
```

Specific assertions exercised:
* `cheri_tag_get(cap) == 1` for every returned cap
* `cap_address ∈ [arena.base, arena.base + arena.size)`
* `cap_length >= requested`
* `(cap_perms & CHERI_PERM_SW_VMEM) == 0`
* Caps from sequential `stopless_alloc` calls do not overlap
* Each cap is fully writable up to its requested length
* OOM returns NULL exactly when bump exceeds capacity
* `stopless_arena_reset` zeroes bump and the next alloc succeeds

### A.2 — `test_alloc_concurrent`

```bash
make CROSS=1 test_alloc_concurrent
scp test_alloc_concurrent cheri-qemu:/root/
ssh cheri-qemu ./test_alloc_concurrent
```

Expected output (4 threads × up to 20k each):
```
test_alloc_concurrent: init
  total_allocs = ~80000  (give or take based on OOM timing)
  oom events   = ~0..4
  cap cursor   = same as total_allocs
  thread 0: ~20000 allocations
  ...
OK  threads=4 total_allocs=N
```

Specific assertions:
* `total == cursor` (no race in array-fill cursor)
* Sorted caps show no overlap
* Bump offset matches expected total (modulo padding warning)

### A.3 — Regression check on existing tests

```bash
make CROSS=1 test_basic test_multi
scp test_basic test_multi cheri-qemu:/root/
ssh cheri-qemu './test_basic && ./test_multi'
```

Both must still pass (we changed `stopless_arena_t` layout; tests use
the struct directly, so verify no breakage).

## Stage B: HotSpot integration

Requires C-1+C-2+C-3 to apply, plus this patch series 0084+0085.

### B.1 — Build hotspot

```bash
cd ~/projs/stopless-java-gc
./scripts/apply_patches.sh
make -C src/cap_runtime/stopless_gc CROSS=1
./scripts/fast_iter.sh
```

Expected: no compile errors. Specifically watch for:
- `error: 'stopless_alloc' was not declared` → allocator.h not in
  include path; check JvmFeatures.gmk
- `error: '_Atomic' does not name a type` → C/C++ macro
  STOPLESS_ATOMIC_SIZE_T not picking up the C++ branch correctly
- `error: 'log_warning' was not declared` → missing
  `#include "logging/log.hpp"` in stoplessHeap.cpp

### B.2 — `java -version` (intersects with C-5/C-6)

```bash
./scripts/run.sh -XX:+UseStoplessGC -Xms16m -Xmx16m -version
```

Expected: JVM starts. If it gets past shift=64 (C-6), proceeds to
class loading; first real allocation calls into our bump allocator.
The `-version` short-circuit may exit before any allocation, in
which case we don't yet exercise the allocator from Java.

To force allocation exercise (after C-5/C-6 unblock):
```bash
./scripts/run.sh -XX:+UseStoplessGC -Xms32m -Xmx32m \
    -Xlog:gc+heap=trace -cp . Hello
```

Where `Hello.java` is:
```java
public class Hello {
  public static void main(String[] args) {
    Object[] a = new Object[1000];
    for (int i = 0; i < a.length; i++) a[i] = new int[16];
    System.out.println("Allocated " + a.length + " arrays");
  }
}
```

Expected: program prints, exits 0. `_arena->used()` grows by
~1000 * 16 * 4 bytes + headers.

### B.3 — OOM path

```bash
./scripts/run.sh -XX:+UseStoplessGC -Xms4m -Xmx4m \
    -cp . Allocate10MB
```

Where `Allocate10MB.java` allocates a 10 MB byte array.

Expected: JVM throws `java.lang.OutOfMemoryError`, exits 1. The
`log_warning(gc)` in `mem_allocate` should fire.

## Sign-off criteria

C-4 ships when:
- [ ] `test_alloc` passes on hasee/QEMU
- [ ] `test_alloc_concurrent` passes on hasee/QEMU
- [ ] `test_basic` and `test_multi` still pass (regression)
- [ ] 0084 + 0085 patches apply on hasee
- [ ] hotspot builds with new sources
- [ ] B.2 (allocation exercise) passes once C-5/C-6 unblock
       `-version`

The C-side tests (A) can sign off independently of the JVM-side
work, and provide functional confidence in the allocator that the
paper relies on.

## Predicted blockers

1. `_Atomic size_t` on hasee's clang — but morello-sdk supports C11
   atomics. Should be fine.
2. CAS-loop on `bump_offset` with extreme contention — improbable
   with 4 threads, but if it spins forever, add exponential backoff
   in the inner loop.
3. The `cheri_representable_length(0)` edge case — guarded by
   `if (size_bytes == 0) return NULL` at function entry.
