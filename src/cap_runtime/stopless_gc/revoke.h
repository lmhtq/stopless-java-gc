/*
 * revoke.h — wrappers around CheriBSD cheri_revoke().
 *
 * The shadow bitmap is the mechanism by which user-space tells the
 * kernel "all caps pointing here are dead, please invalidate them".
 * Each arena gets its own shadow bitmap; we mark bits for the bytes
 * we want revoked, then call cheri_revoke().
 */

#ifndef STOPLESS_GC_REVOKE_H
#define STOPLESS_GC_REVOKE_H

#include <stddef.h>
#include <stdint.h>

/* C++ does not understand C11 _Atomic. C++ TUs that include this
   header only need the LAYOUT of stopless_arena_t (the field is
   touched exclusively from the C side via stopless_alloc / used /
   reset). On aarch64 a plain size_t has the same size and alignment
   as _Atomic size_t, so the ABI matches without C++ ever reading
   the field directly. */
#ifdef __cplusplus
#  define STOPLESS_ATOMIC_SIZE_T size_t
#else
#  include <stdatomic.h>
#  define STOPLESS_ATOMIC_SIZE_T _Atomic size_t
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle: holds the arena cap + its shadow-bitmap cap.
   `bump_offset` (Phase C-4) is the byte cursor consumed by
   stopless_alloc; lock-free CAS keeps it consistent across
   mutator threads. */
typedef struct stopless_arena {
    void     *base;                       /* arena cap, full bounds */
    size_t    size;                       /* arena size in bytes */
    uint64_t *shadow;                     /* shadow-bitmap cap (set bits = revoke) */
    uintptr_t shadow_base_addr;           /* ptraddr_t of arena base, for libcaprevoke API */
    STOPLESS_ATOMIC_SIZE_T bump_offset;   /* C-4: bump-pointer cursor in bytes */
} stopless_arena_t;

/* Map an mmap'd region + acquire its shadow bitmap.
   On success, fills *out and returns 0. */
int  stopless_arena_init(stopless_arena_t *out, size_t size);
void stopless_arena_fini(stopless_arena_t *a);

/* Mark a byte-range [obj_addr, obj_addr+len) for revocation in the
   shadow bitmap. Does NOT trigger the sweep. */
void stopless_mark_revoke(stopless_arena_t *a, uintptr_t obj_addr, size_t len);

/* Clear a previously-set revocation bit (used to "un-mark" before
   actual sweep — rare). */
void stopless_unmark_revoke(stopless_arena_t *a, uintptr_t obj_addr, size_t len);

/* Cap-bounds variant: mark the bounds of `obj_cap` for revocation.
   This matches the cheribsdtest pattern most directly and avoids
   manual address arithmetic. */
int  stopless_mark_revoke_cap(stopless_arena_t *a, void *obj_cap);

/* Trigger the actual kernel revocation sweep (open+close, synchronous,
   blocks the caller for the full heap-linear scan). Returns 0 on success. */
int  stopless_revoke_now(void);

/* Phase-2 split protocol (load-side / CLG barrier):
   - stopless_revoke_open(): OPEN a revocation epoch. Publishes all shadow
     marks set so far and arms the per-page capability-load generation
     barrier: from return onward, no stale capability into a marked range
     can be loaded with its tag intact (the first load from any
     not-yet-scanned page CLG-faults and the kernel lazily sweeps it).
     Costs milliseconds, NO data scan. Call INSIDE the GC pause.
   - stopless_revoke_close(): run the closing scan (LAST_PASS) and wait for
     the epoch to clear. Heap-linear; call OUTSIDE the pause (collector
     thread) — mutators run concurrently under the load-side barrier.
   open() while an epoch is already open is a no-op for the load side; the
   caller must close before marks of a NEW cycle can be published by the
   next open. Returns 0 on success. */
int  stopless_revoke_open(void);
int  stopless_revoke_close(void);

#ifdef __cplusplus
}
#endif

#endif /* STOPLESS_GC_REVOKE_H */
