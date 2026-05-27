/*
 * test_multi.c — multi-object + multi-thread CHERI-Stopless demo.
 *
 * Phase B of the roadmap (docs/15 §5).
 *
 * Setup:
 *   - One source arena (8 MiB) carved into N objects (default 256).
 *   - One destination arena (8 MiB).
 *   - Each object is 1 KiB, contains a magic byte derived from its
 *     index (so we can verify reads return the right object's data).
 *
 * Threads:
 *   - MUTATOR: spins through obj[i] reading byte 0; verifies it
 *     equals magic[i]. Counts total reads + fault retries.
 *   - COLLECTOR: walks obj[0..N-1], for each:
 *       * memcpy obj[i] -> dst[i]
 *       * forward_table_insert(old_addr -> new_cap)
 *       * mark_revoke + revoke_now (per-object granularity for now,
 *         later batch).
 *   - Both run for FIXED_DURATION seconds, then synchronize and check.
 *
 * Expected behaviour:
 *   - Mutator never crashes (handler always forwards).
 *   - Every object's magic byte is observed correctly at least once.
 *   - Total faults ≈ moves × 1 per active mutator (since self-heal
 *     in the register only — first access after move faults, subsequent
 *     accesses succeed because the register cap is the patched one).
 */

#include "../api.h"
#include "../revoke.h"
#include "../forward_table.h"
#include "../handler.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <cheri/cherireg.h>
#include <cheriintrin.h>
#include <stdatomic.h>

#define NUM_OBJS       32
#define OBJ_SIZE       1024
#define ARENA_SIZE     (8 * 1024 * 1024)
#define RUN_SECONDS    3

extern __thread volatile void **stopless_v1_pending_slot;

static stopless_arena_t a_src, a_dst;

/* Per-object slot table: heap-allocated array of cap-typed slots, one
   per object. The mutator reads obj_caps[i] to get the current cap to
   object i. The collector, when moving obj i, updates this slot.
   (In a real GC, slots are the object references in OTHER objects.) */
static char **obj_caps;

/* "Captured" slots: a separate heap-allocated array holding mutator-
   captured-once cap references. Populated by main BEFORE the collector
   starts. The collector revokes the underlying objects; this array is
   what experiences the cap-tag invalidation. Mutator reads from here
   to trigger the slow path. */
static char **captured_slots;

/* Magic byte: byte 0 of obj[i] = magic_of(i). */
static inline unsigned char magic_of(int i) { return (unsigned char)(0xA0 | (i & 0x1F)); }

static atomic_int  stop_flag;
static atomic_ullong mutator_reads;
static atomic_ullong collector_moves;

static void *
mutator_thread(void *arg)
{
    (void)arg;
    /* Use the shared `captured_slots` populated by main BEFORE the
       collector started. This guarantees these slots hold the
       ORIGINAL caps to arena_src, which the collector will revoke. */
    unsigned i = 0;
    while (!atomic_load(&stop_flag)) {
        int idx = i % NUM_OBJS;
        char *p = captured_slots[idx];  /* may be stale-cap after move */
        if (p == NULL) { i++; continue; }
        unsigned char observed = (unsigned char)*p;
        unsigned char expected = magic_of(idx);
        if (observed != expected) {
            fprintf(stderr, "MUTATOR FAIL: obj[%d] observed=0x%02x expected=0x%02x\n",
                    idx, observed, expected);
            atomic_store(&stop_flag, 1);
            return (void *)1;
        }
        atomic_fetch_add(&mutator_reads, 1);
        i++;
    }
    return NULL;
}

static void *
collector_thread(void *arg)
{
    (void)arg;
    /* Each pass: mark+copy all objects, then ONE revoke at the end.
       This batches the kernel sweep cost (~1 per pass vs ~N). */
    int round = 0;
    while (!atomic_load(&stop_flag) && round < 8) {
        /* Phase A: copy + forward + mark (no revoke yet). */
        for (int i = 0; i < NUM_OBJS && !atomic_load(&stop_flag); i++) {
            char *old_cap = obj_caps[i];
            if (old_cap == NULL) continue;
            uintptr_t old_addr = (uintptr_t)cheri_address_get(old_cap);

            /* Each round writes into a distinct slice of arena_dst. */
            char *new_cap = (char *)cheri_bounds_set(
                (char *)a_dst.base + ((round * NUM_OBJS + i) % (ARENA_SIZE / OBJ_SIZE)) * OBJ_SIZE,
                OBJ_SIZE);
            new_cap = (char *)cheri_perms_and(new_cap, ~CHERI_PERM_SW_VMEM);
            memcpy(new_cap, old_cap, OBJ_SIZE);
            forward_table_insert(old_addr, new_cap);
            obj_caps[i] = new_cap;
            stopless_mark_revoke_cap(&a_src, old_cap);
            atomic_fetch_add(&collector_moves, 1);
        }

        /* Phase B: ONE revoke sweep for the whole batch. */
        stopless_revoke_now();

        round++;
    }
    atomic_store(&stop_flag, 1);
    return NULL;
}

int
main(void)
{
    write(2, "test_multi: init\n", 17);
    forward_table_init();
    if (stopless_handler_install() != 0) return 1;
    if (stopless_arena_init(&a_src, ARENA_SIZE) != 0) return 1;
    if (stopless_arena_init(&a_dst, ARENA_SIZE) != 0) return 1;

    /* Allocate per-object slot table (off-arena heap memory). */
    obj_caps = mmap(NULL, NUM_OBJS * sizeof(char *),
                    PROT_READ | PROT_WRITE,
                    MAP_ANON | MAP_PRIVATE, -1, 0);
    if (obj_caps == MAP_FAILED) return 1;

    /* Carve N objects out of arena_src and seed their magic byte. */
    for (int i = 0; i < NUM_OBJS; i++) {
        char *p = (char *)cheri_bounds_set((char *)a_src.base + i * OBJ_SIZE, OBJ_SIZE);
        p = (char *)cheri_perms_and(p, ~CHERI_PERM_SW_VMEM);
        memset(p, magic_of(i), OBJ_SIZE);
        obj_caps[i] = p;
    }
    fprintf(stderr, "Carved %d objects, %zu B each\n", NUM_OBJS, (size_t)OBJ_SIZE);

    /* Pre-populate the "captured_slots" array BEFORE the collector
       starts moving things. These slots hold the original caps; the
       collector will revoke their underlying objects. */
    captured_slots = mmap(NULL, NUM_OBJS * sizeof(char *),
                          PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_PRIVATE, -1, 0);
    if (captured_slots == MAP_FAILED) return 1;
    for (int i = 0; i < NUM_OBJS; i++) captured_slots[i] = obj_caps[i];

    /* Spawn collector + mutator. */
    pthread_t coll, mut;
    atomic_store(&stop_flag, 0);
    if (pthread_create(&coll, NULL, collector_thread, NULL) != 0) return 1;
    if (pthread_create(&mut,  NULL, mutator_thread, NULL) != 0) return 1;

    sleep(RUN_SECONDS);
    atomic_store(&stop_flag, 1);

    void *mut_rc = NULL, *coll_rc = NULL;
    pthread_join(coll, &coll_rc);
    pthread_join(mut,  &mut_rc);

    fprintf(stderr, "=== results ===\n");
    fprintf(stderr, "  collector_moves    = %llu\n", atomic_load(&collector_moves));
    fprintf(stderr, "  mutator_reads      = %llu\n", atomic_load(&mutator_reads));
    fprintf(stderr, "  handler_faults     = %llu\n", stopless_handler_faults);
    fprintf(stderr, "  handler_self_heals = %llu\n", stopless_handler_self_heals);
    fprintf(stderr, "  forward_table_size = %zu\n", forward_table_size());

    int fail = (intptr_t)mut_rc != 0;
    if (atomic_load(&collector_moves) == 0) {
        fprintf(stderr, "FAIL: collector did no moves\n");
        fail = 1;
    }
    if (atomic_load(&mutator_reads) == 0) {
        fprintf(stderr, "FAIL: mutator did no reads\n");
        fail = 1;
    }

    if (!fail) fprintf(stderr, "OK\n");
    return fail;
}
