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

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle: holds the arena cap + its shadow-bitmap cap. */
typedef struct stopless_arena {
    void     *base;             /* arena cap, full bounds */
    size_t    size;             /* arena size in bytes */
    uint64_t *shadow;           /* shadow-bitmap cap (set bits = revoke) */
    uintptr_t shadow_base_addr; /* ptraddr_t of arena base, for libcaprevoke API */
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

/* Trigger the actual kernel revocation sweep. Returns 0 on success. */
int  stopless_revoke_now(void);

#ifdef __cplusplus
}
#endif

#endif /* STOPLESS_GC_REVOKE_H */
