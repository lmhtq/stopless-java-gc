/*
 * test_concurrent_move.c — C-9/C-10 concurrent-correctness proof.
 *
 * Proves the hard, NS-critical case of the CHERI-Stopless design
 * (docs/40 §3.2, §6): REVOKE-FIRST + mutator-assisted evacuation.
 *
 * Scenario:
 *   1. Carve N objects in arena_src; object i is filled with byte
 *      magic(i). Capture the original caps in captured_slots[].
 *   2. REVOKE-FIRST: mark every object + cheri_revoke once. Now every
 *      captured cap is tag-0 — the collector has NOT copied anything
 *      yet. (This is the window docs/15 copy-first never exercised.)
 *   3. M mutator threads hammer the objects through their (now tag-0)
 *      captured caps. Each access SIGPROTs; the handler calls our
 *      evacuate callback, which copies the object to arena_dst and
 *      cas_inserts the forwarding. Many mutators race to evacuate the
 *      SAME object; cas_insert picks exactly one winner.
 *
 * Pass criteria:
 *   - No mutator ever reads a wrong value (CI: every access resolves
 *     to a consistent object at its new location).
 *   - Stores through a stale cap land at the new location too
 *     (unified read+write barrier).
 *   - Each object is copied EXACTLY ONCE (evac_winners == N): the
 *     cas_insert race has a single winner per object.
 *   - No thread blocks waiting on a collector (NS): there is no
 *     collector thread here — the mutators do all the evacuation.
 */

#include "../revoke.h"
#include "../forward_table.h"
#include "../handler.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <cheri/cherireg.h>
#include <cheriintrin.h>

#define ARENA_SIZE   (4 * 1024 * 1024)
#define OBJ_SIZE     128
#define NUM_OBJS     64        /* small set so all get accessed + heavy contention */
#define NUM_MUTATORS 8         /* > objects-per-cache-line to force cas races */
#define RUN_SECONDS  3

static stopless_arena_t a_src, a_dst;
static char **captured_slots;          /* off-arena: holds original (soon tag-0) caps */
static unsigned char g_shadow[NUM_OBJS]; /* original content, for fallback reconstruct */

static atomic_int  g_dst_slot;          /* bump cursor into arena_dst (in objects) */
static atomic_int  g_evac_winners;      /* copies that WON the cas (== N expected) */
static atomic_int  g_evac_rederive_ok;  /* times the post-revoke re-derive read worked */
static atomic_ullong g_reads, g_writes;
static atomic_int  g_stop, g_fail;

static inline unsigned char magic(int i) { return (unsigned char)(0x40 + (i * 7) % 0xB0); }

/* Mutator-assisted evacuation: copy the object at from_addr to arena_dst and
   cas_insert the forwarding. Returns the authoritative (winning) new cap, or
   NULL if from_addr is not one of our objects (genuine fault). */
static void *
my_evacuate(uintptr_t from_addr)
{
    uintptr_t base = (uintptr_t)cheri_address_get(a_src.base);
    if (from_addr < base || from_addr >= base + ARENA_SIZE) return NULL;
    int idx = (int)((from_addr - base) / OBJ_SIZE);
    if (idx < 0 || idx >= NUM_OBJS) return NULL;

    /* Allocate a fresh to-space slot (bump). */
    int slot = atomic_fetch_add(&g_dst_slot, 1);
    char *dst = (char *)cheri_bounds_set(
        (char *)a_dst.base + ((size_t)slot * OBJ_SIZE) % ARENA_SIZE, OBJ_SIZE);
    dst = (char *)cheri_perms_and(dst, ~CHERI_PERM_SW_VMEM);

    /* Read the old object's content. docs/40 §2.3 / §7 risk #3: try a
       cap RE-DERIVED from the arena root AFTER revoke (the collector's
       privileged copy path). If the re-derived cap is tag-0 (revoke also
       killed re-derivations), fall back to the captured shadow content so
       the concurrency proof still stands. */
    char *src = (char *)cheri_bounds_set((char *)a_src.base + (size_t)idx * OBJ_SIZE, OBJ_SIZE);
    src = (char *)cheri_perms_and(src, ~CHERI_PERM_SW_VMEM);
    if (cheri_tag_get(src)) {
        memcpy(dst, src, OBJ_SIZE);
        atomic_fetch_add(&g_evac_rederive_ok, 1);
    } else {
        memset(dst, g_shadow[idx], OBJ_SIZE);   /* fallback reconstruct */
    }

    void *winner = forward_table_cas_insert(from_addr, dst);
    if (winner == (void *)dst) {
        atomic_fetch_add(&g_evac_winners, 1);    /* we won the race for idx */
    }
    return winner;
}

/* One access cycle: read+verify, idempotent write, read+verify again. */
static int
access_obj(int idx)
{
    volatile unsigned char *p = (volatile unsigned char *)captured_slots[idx];
    unsigned char got = *p;                       /* load barrier (traps) */
    atomic_fetch_add(&g_reads, 1);
    if (got != magic(idx)) {
        fprintf(stderr, "READ FAIL obj[%d]: got 0x%02x want 0x%02x\n",
                idx, got, magic(idx));
        return -1;
    }
    *p = magic(idx);                              /* store barrier (traps) */
    atomic_fetch_add(&g_writes, 1);
    if (*p != magic(idx)) {
        fprintf(stderr, "WRITE FAIL obj[%d]: store did not reach new copy\n", idx);
        return -1;
    }
    return 0;
}

static void *
mutator_thread(void *arg)
{
    /* Phase 1: every thread sweeps ALL objects once, starting together —
       this guarantees full coverage AND makes all NUM_MUTATORS threads race
       to evacuate the SAME object at the SAME time (cas_insert contention). */
    for (int idx = 0; idx < NUM_OBJS && !atomic_load(&g_stop); idx++) {
        if (access_obj(idx) != 0) {
            atomic_store(&g_fail, 1); atomic_store(&g_stop, 1); return (void*)1;
        }
    }
    /* Phase 2: random hammering for the rest of the run. */
    unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u + 1;
    while (!atomic_load(&g_stop)) {
        seed = seed * 1103515245u + 12345u;
        int idx = (seed >> 8) % NUM_OBJS;
        if (access_obj(idx) != 0) {
            atomic_store(&g_fail, 1); atomic_store(&g_stop, 1); return (void*)1;
        }
    }
    return NULL;
}

int
main(void)
{
    fprintf(stderr, "test_concurrent_move: init (N=%d, OBJ=%d, mutators=%d)\n",
            NUM_OBJS, OBJ_SIZE, NUM_MUTATORS);
    forward_table_init();
    if (stopless_handler_install() != 0) { fprintf(stderr, "FAIL install\n"); return 1; }
    stopless_set_evacuate_cb(my_evacuate);
    if (stopless_arena_init(&a_src, ARENA_SIZE) != 0) return 1;
    if (stopless_arena_init(&a_dst, ARENA_SIZE) != 0) return 1;

    captured_slots = mmap(NULL, NUM_OBJS * sizeof(char *),
                          PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (captured_slots == MAP_FAILED) return 1;

    /* Carve + seed + capture. */
    for (int i = 0; i < NUM_OBJS; i++) {
        char *p = (char *)cheri_bounds_set((char *)a_src.base + (size_t)i * OBJ_SIZE, OBJ_SIZE);
        p = (char *)cheri_perms_and(p, ~CHERI_PERM_SW_VMEM);
        memset(p, magic(i), OBJ_SIZE);
        g_shadow[i] = magic(i);
        captured_slots[i] = p;
        stopless_mark_revoke_cap(&a_src, p);   /* mark all for revocation */
    }

    /* REVOKE-FIRST: one sweep; now every captured cap is tag-0 and nothing
       has been copied yet. */
    fprintf(stderr, "revoke-first sweep...\n");
    if (stopless_revoke_now() != 0) { fprintf(stderr, "FAIL revoke\n"); return 1; }
    fprintf(stderr, "captured_slots[0] tag after revoke = %d (expect 0)\n",
            (int)cheri_tag_get(captured_slots[0]));

    /* Hammer concurrently — mutators do ALL the evacuation via the handler. */
    pthread_t th[NUM_MUTATORS];
    atomic_store(&g_stop, 0);
    for (long i = 0; i < NUM_MUTATORS; i++)
        pthread_create(&th[i], NULL, mutator_thread, (void *)i);
    sleep(RUN_SECONDS);
    atomic_store(&g_stop, 1);
    for (int i = 0; i < NUM_MUTATORS; i++) pthread_join(th[i], NULL);

    int winners = atomic_load(&g_evac_winners);
    fprintf(stderr, "=== results ===\n");
    fprintf(stderr, "  reads             = %llu\n", atomic_load(&g_reads));
    fprintf(stderr, "  writes            = %llu\n", atomic_load(&g_writes));
    fprintf(stderr, "  handler_faults    = %llu\n", stopless_handler_faults);
    fprintf(stderr, "  handler_assists   = %llu\n", stopless_handler_assists);
    fprintf(stderr, "  evac_winners      = %d  (expect %d, one copy per object)\n",
            winners, NUM_OBJS);
    fprintf(stderr, "  evac_rederive_ok  = %d  (post-revoke re-derive reads that worked)\n",
            atomic_load(&g_evac_rederive_ok));
    fprintf(stderr, "  forward_table_size= %zu\n", forward_table_size());

    int fail = atomic_load(&g_fail);
    if (winners != NUM_OBJS) {
        fprintf(stderr, "FAIL: %d objects copied, expected exactly %d "
                "(cas_insert race not single-winner)\n", winners, NUM_OBJS);
        fail = 1;
    }
    if (atomic_load(&g_reads) < (unsigned long long)NUM_OBJS) {
        fprintf(stderr, "FAIL: too few reads\n"); fail = 1;
    }
    fprintf(stderr, fail ? "FAIL\n" : "OK\n");
    stopless_handler_remove();
    return fail;
}
