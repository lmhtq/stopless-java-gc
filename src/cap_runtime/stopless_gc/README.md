# cap_runtime/stopless_gc — CHERI-Stopless GC primitives

Standalone C/C++ implementation of the CHERI-Stopless GC mechanism
described in `docs/15`. Buildable and testable on CheriBSD-purecap
without OpenJDK.

## Files

| File | Purpose |
|---|---|
| `revoke.{h,c}` | wrapper around CheriBSD `cheri_revoke` syscall + shadow-bitmap helpers |
| `forward_table.{h,c}` | from-cap → to-cap mapping (RCU-friendly hash map) |
| `rebuild_cap.{h,c}` | reconstruct a valid cap from `(addr, bounds, perms)` |
| `handler.{h,c}` | SIGPROT handler: lookup forwarding, rematerialize, self-heal |
| `api.h` | public interface for JVM integration |
| `Makefile` | builds `libstopless_gc.a` + tests |
| `tests/` | functional tests on QEMU |

## Building

```bash
# On hasee (cross-compile to CheriBSD purecap):
make CC=$MORELLO_SDK/bin/clang \
     CFLAGS="-march=morello+c64 -mabi=purecap --target=aarch64-unknown-freebsd \
             --sysroot=$CHERIBSD_SYSROOT"

# Or natively on a CheriBSD/QEMU guest:
make
```

## Tests

| Test | Validates |
|---|---|
| `test_basic.c` | revoke + rebuild + cap_load fault cycle, single object |
| `test_forwarding.c` | full mover loop: allocate, move N objects, fault-walk |
| `test_concurrent.c` | mutator thread + collector thread interleaved (no JVM) |

## What this is NOT

* Not a complete GC — it's the *mechanism* layer.
* No allocator, no Java integration, no mark phase.
* Just the cap-revocation + fault-handling primitive that makes
  CHERI-Stopless work.

The OpenJDK integration lives in `src/hotspot/share/gc/stopless/`
(future patch series 0080+).
