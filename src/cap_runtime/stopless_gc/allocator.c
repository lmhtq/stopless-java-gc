/*
 * allocator.c — lock-free bump-pointer allocator with CHERI bounds.
 *
 * Phase C-4 — see docs/c4/design.md.
 *
 * Algorithm:
 *   1. Atomic CAS-loop on arena->bump_offset.
 *   2. For each candidate offset, align UP to CHERI representable
 *      alignment for the requested size. (Most JVM objects are <4 KB
 *      so this is a no-op on Morello.)
 *   3. Reserve [aligned_off, aligned_off + rounded) where `rounded`
 *      is cheri_representable_length(size).
 *   4. Derive the object cap with cheri_bounds_set(size) and strip
 *      CHERI_PERM_SW_VMEM so the kernel sweep can later revoke it.
 *
 * Notes:
 *   - We round the EXTENT of the reservation up to representable
 *     length (otherwise the next allocation could overlap the
 *     enlarged cap). We then set bounds to the user-requested size
 *     so the cap is tight to the object, even though the arena
 *     space "behind" it (extent - size) is wasted. On a fresh
 *     allocation this is fine; subsequent reuse after revoke
 *     reclaims it implicitly.
 *   - The bounds.length field of the returned cap is therefore
 *     >= size but may be > size if CHERI precision forces it.
 *     Callers that rely on cap_length must account for this.
 */

#include "allocator.h"
#include "revoke.h"

#include <cheriintrin.h>
#include <cheri/cherireg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>

static inline uintptr_t
align_up_repr(uintptr_t off, size_t rounded_size)
{
    /* For requested-length L, the CHERI representable alignment mask
       returned by cheri_representable_alignment_mask(L) has the bits
       that must be ZERO in the base address. */
    size_t mask = ~cheri_representable_alignment_mask(rounded_size);
    return (off + mask) & ~mask;
}

void *
stopless_alloc(void *arena_, size_t size_bytes)
{
    stopless_arena_t *a = (stopless_arena_t *)arena_;
    if (a == NULL || a->base == NULL || size_bytes == 0) return NULL;

    size_t rounded = cheri_representable_length(size_bytes);
    if (rounded < size_bytes) {
        /* overflow — shouldn't happen with reasonable sizes */
        return NULL;
    }

    size_t cur = atomic_load_explicit(&a->bump_offset, memory_order_relaxed);
    size_t aligned_off, new_off;

    for (;;) {
        aligned_off = (size_t)align_up_repr((uintptr_t)cur, rounded);
        new_off     = aligned_off + rounded;
        if (new_off > a->size) {
            return NULL;  /* OOM */
        }
        if (atomic_compare_exchange_weak_explicit(
                &a->bump_offset, &cur, new_off,
                memory_order_acq_rel, memory_order_relaxed)) {
            break;
        }
        /* CAS failed: cur was updated to current value; retry. */
    }

    char *obj = (char *)a->base + aligned_off;
    obj = (char *)cheri_bounds_set(obj, size_bytes);
    obj = (char *)cheri_perms_and(obj, ~CHERI_PERM_SW_VMEM);
    return obj;
}

size_t
stopless_arena_used(stopless_arena_t *a)
{
    if (a == NULL) return 0;
    return atomic_load_explicit(&a->bump_offset, memory_order_acquire);
}

void
stopless_arena_reset(stopless_arena_t *a)
{
    if (a == NULL) return;
    atomic_store_explicit(&a->bump_offset, 0, memory_order_release);
}
