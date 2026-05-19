# Cornucopia / cheri_revoke API Survey

**Status**: R3 source-analyzed against `CTSRD-CHERI/cheribsd` (main
branch as of 2026-05). Findings pre-fill what the spike would
otherwise have discovered.

**Method**: Sparse clone of `cheribsd` on
`bc@hasee:~/projs/stopless-java-gc-analysis/cheribsd-src/cheribsd/`,
covering `sys/cheri/`, `sys/kern/`, `lib/libcheri/`, `sys/sys/`.
Files inspected: full `sys/cheri/revoke.h`.

## 1. Top-line finding

**The CheriBSD revoke API is arena-based and works with any
caller-supplied memory region, not just libc-malloc-managed pages.**
The JVM, which manages its own heap independently of `malloc`, can
drive the revoke state machine directly. **R3 is substantially
retired in our favor.**

## 2. Public API surface

From `sys/cheri/revoke.h`:

```c
/* Drive the revocation state machine. */
int cheri_revoke(int flags,
                 cheri_revoke_epoch_t start_epoch,
                 struct cheri_revoke_syscall_info *crsi);

/* Request a capability to the shadow bitmap for the given arena. */
int cheri_revoke_get_shadow(int flags,
                            void * __capability arena,
                            void * __capability *shadow);
```

Two syscalls. The first drives a revocation pass; the second returns
a capability to a shadow bitmap covering an arena that the caller owns.

### 2.1 Flags for `cheri_revoke`

```c
#define CHERI_REVOKE_LAST_PASS      0x0001  /* finalize epoch */
#define CHERI_REVOKE_IGNORE_START   0x0004  /* advance epoch unconditionally */
#define CHERI_REVOKE_ASYNC          0x0020  /* offload scan to a kernel worker */
#define CHERI_REVOKE_TAKE_STATS     0x1000  /* reset stats counters after report */
```

### 2.2 Shadow space variants

`cheri_revoke_get_shadow` takes a flag selecting which shadow:

```c
#define CHERI_REVOKE_SHADOW_NOVMEM        0x00  /* generic shadow */
#define CHERI_REVOKE_SHADOW_OTYPE         0x01  /* otype shadow */
#define CHERI_REVOKE_SHADOW_INFO_STRUCT   0x03  /* R/O shared state */
#define CHERI_REVOKE_SHADOW_NOVMEM_ENTIRE 0x07  /* entire shadow region */
```

**For our use, `CHERI_REVOKE_SHADOW_NOVMEM` is the right ask** — it
yields a cap to the shadow bitmap covering a non-VM-managed arena.
The JVM heap, allocated as one or more large mmap regions, qualifies.

### 2.3 Epoch model

```c
typedef uint64_t cheri_revoke_epoch_t;

struct cheri_revoke_epochs {
    cheri_revoke_epoch_t enqueue;  /* on entry to quarantine */
    cheri_revoke_epoch_t dequeue;  /* gate for removal */
};
```

Epochs serialize. A region marked at epoch N is safe to physically
reuse once `cheri_revoke_epoch_clears(now, N)` returns true. The
epoch arithmetic uses RFC1982 serial-number ordering.

### 2.4 Stats

`struct cheri_revoke_stats` (truncated, see header for full):

```c
struct cheri_revoke_stats {
    uint64_t page_scan_cycles;       /* cycles in MD page-scan */
    uint64_t fault_cycles;           /* cycles in MI CLG handler */
    uint32_t pages_scan_ro;
    uint32_t pages_scan_rw;
    uint32_t pages_faulted_ro;
    uint32_t pages_faulted_rw;
    uint32_t fault_visits;
    uint32_t caps_found;
    uint32_t caps_found_revoked;
    uint32_t caps_cleared;           /* this pass */
    uint32_t pages_mark_clean;       /* Cornucopia heritage */
};
```

Excellent for our paper — these are exactly the per-pass metrics we
need for the Phase 2 evaluation section.

## 3. Driver pattern from JVM perspective

The natural Phase 2 integration:

```c
// Once, at JVM start:
void * __capability heap_arena = /* cap covering the Java heap mmap */;
void * __capability shadow;
cheri_revoke_get_shadow(CHERI_REVOKE_SHADOW_NOVMEM,
                        heap_arena, &shadow);

// When ZGC begins evacuating region R:
mark_pages_in_shadow(shadow, R.base, R.size);

// Drive the state machine:
struct cheri_revoke_syscall_info info = {};
cheri_revoke(CHERI_REVOKE_ASYNC, current_epoch, &info);

// Mutator load barriers now trap on stale caps to revoked pages.
// SIGCAPRVOKE handler consults forwarding table, self-heals.

// When ZGC wants to reuse R's physical memory:
cheri_revoke(CHERI_REVOKE_LAST_PASS, current_epoch, &info);
// Wait for cheri_revoke_epoch_clears(info.epochs.dequeue, R.epoch_at_revoke).
// Then it is safe to recycle the pages.
```

This maps cleanly to our `src/cap_runtime/revoke_glue.{h,cc}` API:

| `revoke_glue` method | Phase 2 implementation |
|---|---|
| `RevokeDriver::revoke_region(R)` | `mark_pages_in_shadow(shadow, R)` + `cheri_revoke(CHERI_REVOKE_ASYNC, ...)` |
| `RevokeDriver::quiesce()` | poll `cheri_revoke(0, current, &info)` until `epoch_clears` |
| `RevokeDriver::on_sigcaprevoke` | `siginfo_t` extraction + forwarding-table lookup |

## 4. Revoke detection

The `cheri_revoke_is_revoked` helper documents what the LSU does on a
load:

```c
static inline int
cheri_revoke_is_revoked(const void * __capability cap) {
    int is_revoked;
    is_revoked = (cheri_tag_get(cap) == 0);
#ifndef CHERI_CAPREVOKE_CLEARTAGS
    is_revoked |= (cheri_perms_get(cap) == 0);
#endif
    return is_revoked;
}
```

Two modes: either revocation clears the cap tag (CLEARTAGS build), or
zeros all permission bits (default). For our purposes, both modes
trap on first use — only the trap signature in the handler differs.

## 5. R3 verdict

**R3 is downgraded from Medium to Low.**

The API surface is JVM-friendly. The shadow bitmap is per-arena and
the JVM heap qualifies as an arena. The epoch model gives us the
quiescence detection we need to recycle physical pages safely. The
stats struct gives us exactly the numbers we need for the paper.

**No kernel patch is required.** The earlier mitigation budget of
"500-1500 LOC of CheriBSD patch if API isn't JVM-friendly" can be
deleted.

**Remaining real R3 risk**:

- The shadow-bitmap representable-bounds rule: "This call must fail
  if the resulting capability would not be representable due to
  alignment constraints." The Java heap is typically multi-gigabyte
  and page-aligned, so this should be a no-op in practice. Spike day
  11 confirms by trying it.
- The cost of `cheri_revoke(CHERI_REVOKE_ASYNC, ...)` is documented
  in stats but not in absolute time. Cornucopia Reloaded's published
  numbers suggest per-page costs are low; we will measure our own.

## 6. References

- `sys/cheri/revoke.h` (CheriBSD main, ~370 lines)
- `sys/kern/kern_cheri_revoke.c` (implementation, not deep-read)
- Filardo et al., *Cornucopia Reloaded* (ASPLOS 2024) — academic
  exposition of the same machinery.
- `docs/02_phase_ii_cheri_barrier.md §3` — Phase 2 design, now
  confirmed against the actual API.
