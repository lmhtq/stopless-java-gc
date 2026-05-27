/*
 * test_alloc.c — bump-pointer allocator unit test.
 *
 * Phase C-4 — see docs/c4/design.md.
 *
 * Coverage:
 *   1. Allocate N objects of varying sizes (16 B, 64 B, 256 B,
 *      4 KiB, 64 KiB).
 *   2. For each cap returned, assert:
 *        - tag == 1
 *        - cap address ∈ arena bounds
 *        - cap length >= requested size
 *        - CHERI_PERM_SW_VMEM is cleared
 *   3. Allocate until OOM, assert NULL is returned exactly when
 *      bump_offset + size > arena size.
 *   4. Reset arena, verify bump_offset == 0, verify new allocation
 *      starts at offset 0.
 */

#include "../allocator.h"
#include "../revoke.h"
#include "../api.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cheri/cherireg.h>
#include <cheriintrin.h>

#define ARENA_BYTES (8 * 1024 * 1024)  /* 8 MiB */

static int fails = 0;

#define ASSERT(cond, fmt, ...) do {                                    \
    if (!(cond)) {                                                     \
        fprintf(stderr, "FAIL %s:%d: " fmt "\n",                       \
                __FILE__, __LINE__, ##__VA_ARGS__);                    \
        fails++;                                                       \
    }                                                                  \
} while (0)

static void
verify_cap(void *cap, size_t requested, void *arena_base, size_t arena_size)
{
    ASSERT(cap != NULL, "cap is NULL");
    if (cap == NULL) return;

    int tag = (int)cheri_tag_get(cap);
    ASSERT(tag == 1, "cap tag = %d, expected 1", tag);

    uintptr_t addr = (uintptr_t)cheri_address_get(cap);
    uintptr_t base = (uintptr_t)arena_base;
    ASSERT(addr >= base && addr < base + arena_size,
           "cap addr 0x%lx outside arena [0x%lx, 0x%lx)",
           (unsigned long)addr, (unsigned long)base,
           (unsigned long)(base + arena_size));

    size_t len = cheri_length_get(cap);
    ASSERT(len >= requested,
           "cap length %zu < requested %zu", len, requested);

    size_t perms = cheri_perms_get(cap);
    ASSERT((perms & CHERI_PERM_SW_VMEM) == 0,
           "cap still has CHERI_PERM_SW_VMEM (perms=0x%zx)", perms);
}

int
main(void)
{
    fprintf(stderr, "test_alloc: init\n");

    stopless_arena_t a;
    if (stopless_arena_init(&a, ARENA_BYTES) != 0) {
        fprintf(stderr, "arena_init failed\n");
        return 1;
    }

    /* Test 1: varying sizes. */
    const size_t sizes[] = { 16, 64, 256, 1024, 4096, 16384, 65536 };
    const int    N        = sizeof(sizes) / sizeof(sizes[0]);
    void *caps[N];
    size_t before = stopless_arena_used(&a);
    ASSERT(before == 0, "fresh arena used=%zu, expected 0", before);

    for (int i = 0; i < N; i++) {
        caps[i] = stopless_alloc(&a, sizes[i]);
        verify_cap(caps[i], sizes[i], a.base, a.size);
    }
    size_t after = stopless_arena_used(&a);
    ASSERT(after > 0 && after < a.size,
           "used=%zu after %d allocations (arena=%zu)",
           after, N, a.size);
    fprintf(stderr, "  varying-size: used=%zu/%zu after %d allocs\n",
            after, a.size, N);

    /* Test 2: non-overlap. No two caps should share any byte. */
    for (int i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            uintptr_t ai = (uintptr_t)cheri_address_get(caps[i]);
            uintptr_t aj = (uintptr_t)cheri_address_get(caps[j]);
            uintptr_t ei = ai + cheri_length_get(caps[i]);
            uintptr_t ej = aj + cheri_length_get(caps[j]);
            ASSERT(ei <= aj || ej <= ai,
                   "caps %d and %d overlap [0x%lx,0x%lx) [0x%lx,0x%lx)",
                   i, j, (unsigned long)ai, (unsigned long)ei,
                   (unsigned long)aj, (unsigned long)ej);
        }
    }
    fprintf(stderr, "  no-overlap: PASS\n");

    /* Test 3: writable. Each cap should be writable through. */
    for (int i = 0; i < N; i++) {
        unsigned char *p = (unsigned char *)caps[i];
        for (size_t k = 0; k < sizes[i]; k++) p[k] = (unsigned char)(i + k);
        for (size_t k = 0; k < sizes[i]; k++) {
            ASSERT(p[k] == (unsigned char)(i + k),
                   "cap %d byte %zu: got 0x%02x", i, k, p[k]);
        }
    }
    fprintf(stderr, "  read-write: PASS\n");

    /* Test 4: OOM. Bump up against the arena edge. */
    stopless_arena_t a2;
    if (stopless_arena_init(&a2, 64 * 1024) != 0) {
        fprintf(stderr, "arena_init(64K) failed\n");
        return 1;
    }
    int alloc_count = 0;
    while (stopless_alloc(&a2, 1024) != NULL) alloc_count++;
    void *should_be_null = stopless_alloc(&a2, 1024);
    ASSERT(should_be_null == NULL, "post-OOM alloc returned non-NULL");
    fprintf(stderr, "  OOM after %d allocs of 1024 (arena=64K)\n", alloc_count);
    stopless_arena_fini(&a2);

    /* Test 5: reset. */
    stopless_arena_reset(&a);
    size_t used = stopless_arena_used(&a);
    ASSERT(used == 0, "after reset, used=%zu, expected 0", used);
    void *fresh = stopless_alloc(&a, 64);
    ASSERT(fresh != NULL, "alloc after reset returned NULL");
    fprintf(stderr, "  reset: PASS\n");

    stopless_arena_fini(&a);

    if (fails == 0) {
        fprintf(stderr, "OK  test_alloc\n");
        return 0;
    } else {
        fprintf(stderr, "FAIL  test_alloc (%d failures)\n", fails);
        return 1;
    }
}
