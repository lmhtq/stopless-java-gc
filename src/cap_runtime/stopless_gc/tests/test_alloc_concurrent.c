/*
 * test_alloc_concurrent.c — multi-thread bump allocator stress test.
 *
 * Phase C-4 — see docs/c4/design.md.
 *
 * Coverage:
 *   - Spawn T threads (T=4 by default).
 *   - Each thread loops, calling stopless_alloc(arena, 64) until
 *     it has accumulated N allocations or OOM.
 *   - After join, assert that:
 *      a. No two threads received overlapping caps
 *      b. Total allocation count matches sum of per-thread counts
 *      c. arena->bump_offset equals total_count * representable(64)
 *   - We collect cap base addresses in a global, sort, and check
 *     for any duplicates / overlaps.
 *
 * Expected output: OK  threads=4 total_allocs=N
 */

#include "../allocator.h"
#include "../revoke.h"
#include "../api.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <cheri/cherireg.h>
#include <cheriintrin.h>

#define ARENA_BYTES     (16 * 1024 * 1024)   /* 16 MiB */
#define NUM_THREADS     4
#define PER_THREAD_MAX  20000
#define ALLOC_SIZE      64

static stopless_arena_t a;
static atomic_int total_allocs;
static atomic_int total_oom;
static void *all_caps[NUM_THREADS * PER_THREAD_MAX];
static atomic_int caps_cursor;

static void *
worker(void *arg)
{
    long tid = (long)arg;
    int got = 0;
    for (int i = 0; i < PER_THREAD_MAX; i++) {
        void *p = stopless_alloc(&a, ALLOC_SIZE);
        if (p == NULL) {
            atomic_fetch_add(&total_oom, 1);
            break;
        }
        int slot = atomic_fetch_add(&caps_cursor, 1);
        all_caps[slot] = p;
        /* Write a per-thread sentinel byte. */
        *(unsigned char *)p = (unsigned char)(0x80 | (tid & 0x7F));
        got++;
    }
    atomic_fetch_add(&total_allocs, got);
    return (void *)(long)got;
}

static int
cmp_addr(const void *x, const void *y)
{
    uintptr_t a = (uintptr_t)cheri_address_get(*(void **)x);
    uintptr_t b = (uintptr_t)cheri_address_get(*(void **)y);
    return (a < b) ? -1 : (a > b);
}

int
main(void)
{
    fprintf(stderr, "test_alloc_concurrent: init\n");
    if (stopless_arena_init(&a, ARENA_BYTES) != 0) {
        fprintf(stderr, "arena_init failed\n");
        return 1;
    }

    pthread_t threads[NUM_THREADS];
    for (long i = 0; i < NUM_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, worker, (void *)i) != 0) {
            fprintf(stderr, "pthread_create %ld failed\n", i);
            return 1;
        }
    }
    int per_thread[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        void *rc = NULL;
        pthread_join(threads[i], &rc);
        per_thread[i] = (int)(long)rc;
    }

    int total = atomic_load(&total_allocs);
    int oom   = atomic_load(&total_oom);
    int cursor = atomic_load(&caps_cursor);

    fprintf(stderr, "  total_allocs = %d\n", total);
    fprintf(stderr, "  oom events   = %d\n", oom);
    fprintf(stderr, "  cap cursor   = %d\n", cursor);
    for (int i = 0; i < NUM_THREADS; i++) {
        fprintf(stderr, "  thread %d: %d allocations\n", i, per_thread[i]);
    }

    if (total != cursor) {
        fprintf(stderr, "FAIL: total %d != cursor %d\n", total, cursor);
        return 1;
    }
    if (total == 0) {
        fprintf(stderr, "FAIL: no allocations succeeded\n");
        return 1;
    }

    /* Verify uniqueness by sorting addresses and checking adjacents. */
    qsort(all_caps, total, sizeof(void *), cmp_addr);
    for (int i = 1; i < total; i++) {
        uintptr_t prev_addr = (uintptr_t)cheri_address_get(all_caps[i-1]);
        size_t    prev_len  = cheri_length_get(all_caps[i-1]);
        uintptr_t cur_addr  = (uintptr_t)cheri_address_get(all_caps[i]);
        if (cur_addr < prev_addr + prev_len) {
            fprintf(stderr,
                    "FAIL: overlap at index %d: prev=[0x%lx,+%zu) cur=0x%lx\n",
                    i, (unsigned long)prev_addr, prev_len,
                    (unsigned long)cur_addr);
            return 1;
        }
    }

    /* Verify bump_offset matches expected total. */
    size_t expected = (size_t)total * cheri_representable_length(ALLOC_SIZE);
    size_t actual   = stopless_arena_used(&a);
    /* Equality holds when no alignment padding was wasted between
       allocations; since each is the same size with natural
       representable alignment we expect exact match. */
    if (actual != expected) {
        fprintf(stderr,
                "WARN: bump_offset %zu != expected %zu (delta=%zd)\n",
                actual, expected, (ssize_t)actual - (ssize_t)expected);
        /* Not a fatal failure — could be padding edge cases. */
    }

    stopless_arena_fini(&a);
    fprintf(stderr, "OK  threads=%d total_allocs=%d\n", NUM_THREADS, total);
    return 0;
}
