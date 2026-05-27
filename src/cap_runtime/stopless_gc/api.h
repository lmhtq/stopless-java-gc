/*
 * cap_runtime/stopless_gc/api.h — public interface.
 *
 * Three layers of API are exposed:
 *
 *   (1) Heap setup:   stopless_arena_create()
 *   (2) Mover side:   stopless_move(), stopless_revoke()
 *   (3) Mutator side: handler is installed once, runs on SIGPROT
 *
 * The JVM (or test harness) drives layers 1 and 2. Layer 3 is
 * automatic once stopless_init() has been called.
 *
 * All caps in this API are CHERI capabilities; on non-CHERI builds
 * they degenerate to void*.
 */

#ifndef STOPLESS_GC_API_H
#define STOPLESS_GC_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Initialisation ------------------------------------------------------ */

/* Install the SIGPROT handler and initialise the global forwarding table.
   Idempotent. Returns 0 on success. */
int  stopless_init(void);
void stopless_shutdown(void);

/* -- Arena management ---------------------------------------------------- */

/* Create a heap arena of `size` bytes. Returns a cap with bounds set
   to exactly [base, base+size). The arena is the source of all
   object caps. Returns NULL on failure. */
void *stopless_arena_create(size_t size);

/* Destroy an arena. All outstanding caps to it should already have
   been revoked. */
void stopless_arena_destroy(void *arena);

/* -- Allocator ----------------------------------------------------------- */

/* Bump-pointer allocate `size` bytes from arena. Returns a cap with
   bounds tight to [obj, obj+size). The cap is the only valid handle
   to the object. */
void *stopless_alloc(void *arena, size_t size);

/* -- Mover --------------------------------------------------------------- */

/* Move object at `from_cap` to a new location in `to_arena`.
   Steps:
     1. Allocate object-sized space in to_arena.
     2. memcpy (cap-tag preserving) from from_cap to new location.
     3. Insert forwarding-table entry: from_addr -> new_cap.
     4. Mark from_addr's revocation bit in the shadow bitmap.
   Returns the new cap (caller may install it, e.g. to update roots
   that the collector reaches directly).
   The OLD cap is not invalidated yet; subsequent revoke sweep does that. */
void *stopless_move(void *from_cap, void *to_arena);

/* Trigger the actual revocation sweep. After this returns, every cap
   to a moved object that hasn't been forwarded is tag-zero. Subsequent
   mutator loads of such caps will SIGPROT into our handler. */
int  stopless_revoke_sweep(void);

/* -- Stats (read-only) --------------------------------------------------- */

struct stopless_stats {
    uint64_t moves_completed;     /* objects copied */
    uint64_t faults_handled;      /* SIGPROT events forwarded */
    uint64_t self_heal_writes;    /* slots updated by handler */
    uint64_t forwarding_entries;  /* current size of forwarding table */
    uint64_t revoke_sweeps;       /* number of revoke calls */
};

void stopless_stats_read(struct stopless_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* STOPLESS_GC_API_H */
