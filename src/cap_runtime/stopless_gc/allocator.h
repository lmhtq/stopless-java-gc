/*
 * allocator.h — bump-pointer allocator for stopless_arena_t.
 *
 * Lock-free CAS-based; safe for concurrent mutator allocation.
 * Each returned cap has tight bounds (cheri_bounds_set) and has
 * CHERI_PERM_SW_VMEM stripped so the kernel revocation sweep will
 * later invalidate it (cf. cheribsdtest_vm.c).
 *
 * Phase C-4 — see docs/c4/design.md.
 */

#ifndef STOPLESS_GC_ALLOCATOR_H
#define STOPLESS_GC_ALLOCATOR_H

#include "revoke.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate `size` bytes from `arena`. Returns a cap with bounds
   tight to [obj, obj+size) and CHERI_PERM_SW_VMEM stripped, or NULL
   on OOM. Thread-safe (CAS on bump offset). */
void *stopless_alloc(void *arena, size_t size);

/* Current bump offset (bytes consumed from the arena). For
   diagnostics and StoplessArena::used(). */
size_t stopless_arena_used(stopless_arena_t *a);

/* Reset the bump offset to 0. Caller responsibility to ensure no
   live caps point inside (after a full revoke sweep). Used by the
   collector when an arena has been fully evacuated. */
void   stopless_arena_reset(stopless_arena_t *a);

#ifdef __cplusplus
}
#endif

#endif /* STOPLESS_GC_ALLOCATOR_H */
