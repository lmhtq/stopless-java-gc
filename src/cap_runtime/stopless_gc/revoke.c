/*
 * revoke.c — implementation of the cheri_revoke / shadow-bitmap wrappers.
 */

#include "revoke.h"

#include <sys/mman.h>
#include <cheri/revoke.h>
#include <cheri/libcaprevoke.h>
#include <cheriintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>

/* Process-wide CHERI revocation info struct. Lazily acquired from the
   kernel via cheri_revoke_get_shadow(SHADOW_INFO_STRUCT). Holds the
   `base_mem_nomap` ptraddr that all caprev_shadow_nomap_* calls need. */
static volatile const struct cheri_revoke_info *g_cri;

static int
ensure_cri(void)
{
    if (g_cri != NULL) return 0;
    void *p = NULL;
    int rc = cheri_revoke_get_shadow(CHERI_REVOKE_SHADOW_INFO_STRUCT, NULL, &p);
    if (rc != 0) {
        fprintf(stderr, "ensure_cri: cheri_revoke_get_shadow(INFO) failed: %s\n",
                strerror(errno));
        return -1;
    }
    g_cri = (const struct cheri_revoke_info *)p;
    return 0;
}

int
stopless_arena_init(stopless_arena_t *out, size_t size)
{
    if (out == NULL || size == 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    /* Map the arena. PROT_MAX wired wide so we can later mprotect for
       write barriers (§3.6 of design).
       PROT_CAP is required for the arena to accept cap stores (i.e.,
       store-via-cap of a tagged capability such as a Java oop). Without
       it, mmap returns a cap whose perms lack STORE_CAP, and the first
       oop-store through such a cap SIGPROTs (CheriBSD mman.h:PROT_CAP).
       test_basic/test_alloc happened not to exercise cap-stores into the
       arena, so they passed despite the missing perm — JVM Throwable's
       static-final-String init was the first observable trigger. */
    void *p = mmap(NULL, size,
                   PROT_READ | PROT_WRITE | PROT_CAP |
                   PROT_MAX(PROT_READ | PROT_WRITE | PROT_CAP),
                   MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "stopless_arena_init: mmap failed: %s\n", strerror(errno));
        return -1;
    }
    out->base = p;
    out->size = size;

    if (ensure_cri() != 0) {
        munmap(p, size);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    out->shadow_base_addr = g_cri->base_mem_nomap;

    /* Acquire the shadow bitmap for this arena. */
    int rc = cheri_revoke_get_shadow(CHERI_REVOKE_SHADOW_NOVMEM,
                                     p, (void **)&out->shadow);
    if (rc != 0) {
        fprintf(stderr, "stopless_arena_init: cheri_revoke_get_shadow failed: %s\n",
                strerror(errno));
        munmap(p, size);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}

void
stopless_arena_fini(stopless_arena_t *a)
{
    if (a == NULL || a->base == NULL) return;
    munmap(a->base, a->size);
    /* Shadow bitmap is kernel-owned; nothing to free. */
    memset(a, 0, sizeof(*a));
}

void
stopless_mark_revoke(stopless_arena_t *a, uintptr_t obj_addr, size_t len)
{
    if (a == NULL || a->shadow == NULL || len == 0) return;
    /* set_raw signature: (sbase, sb, heap_START, heap_END). The last
       two are absolute addresses, NOT (start, length). Convert. */
    caprev_shadow_nomap_set_raw(a->shadow_base_addr, a->shadow,
                                obj_addr, obj_addr + len);
}

void
stopless_unmark_revoke(stopless_arena_t *a, uintptr_t obj_addr, size_t len)
{
    if (a == NULL || a->shadow == NULL || len == 0) return;
    caprev_shadow_nomap_clear_raw(a->shadow_base_addr, a->shadow,
                                  obj_addr, obj_addr + len);
}

int
stopless_mark_revoke_cap(stopless_arena_t *a, void *obj_cap)
{
    if (a == NULL || a->shadow == NULL || obj_cap == NULL) return -1;
    /* The `caprev_shadow_nomap_set` variant uses the cap's BOUNDS for
       marking, not explicit (addr, len) arithmetic. Used by
       cheribsdtest. */
    return caprev_shadow_nomap_set(a->shadow_base_addr, a->shadow,
                                   obj_cap, obj_cap);
}

int
stopless_revoke_now(void)
{
    if (ensure_cri() != 0) return -1;

    atomic_thread_fence(memory_order_acq_rel);
    fprintf(stderr, "[stopless] epochs before: enq=%llu deq=%llu\n",
            (unsigned long long)g_cri->epochs.enqueue,
            (unsigned long long)g_cri->epochs.dequeue);

    cheri_revoke_epoch_t initial = g_cri->epochs.enqueue;
    int rc = cheri_revoke(CHERI_REVOKE_IGNORE_START | CHERI_REVOKE_LAST_PASS,
                          initial, NULL);
    fprintf(stderr, "[stopless] cheri_revoke #1 rc=%d errno=%d epochs: enq=%llu deq=%llu\n",
            rc, errno,
            (unsigned long long)g_cri->epochs.enqueue,
            (unsigned long long)g_cri->epochs.dequeue);
    if (rc != 0) return rc;

    cheri_revoke_epoch_t target = g_cri->epochs.enqueue;
    int loops = 0;
    while (!cheri_revoke_epoch_clears(g_cri->epochs.dequeue, target) && loops < 10) {
        rc = cheri_revoke(CHERI_REVOKE_LAST_PASS, target, NULL);
        loops++;
        fprintf(stderr, "[stopless] cheri_revoke wait #%d rc=%d enq=%llu deq=%llu target=%llu\n",
                loops, rc,
                (unsigned long long)g_cri->epochs.enqueue,
                (unsigned long long)g_cri->epochs.dequeue,
                (unsigned long long)target);
        if (rc != 0) return rc;
    }
    /* FAIL-SAFE: the wait loop is bounded; report failure if the epoch
       never cleared instead of unconditionally claiming success. */
    return cheri_revoke_epoch_clears(g_cri->epochs.dequeue, target) ? 0 : -1;
}

/* Phase-2 split protocol. See revoke.h. Quiet by default
   (STOPLESS_REVOKE_VERBOSE=1 to trace). */
static int g_rv_verbose = -1;
static int
rv_verbose(void)
{
    if (g_rv_verbose < 0)
        g_rv_verbose = (getenv("STOPLESS_REVOKE_VERBOSE") != NULL) ? 1 : 0;
    return g_rv_verbose;
}

/* Fault injection for fail-safe-path testing: STOPLESS_FAULT_OPEN=N /
   STOPLESS_FAULT_CLOSE=N make the first N calls fail without touching the
   kernel, exercising the JVM-side fallback / retry paths. */
static int
fault_budget(const char *env, int *counter)
{
    if (*counter == -2) {
        const char *v = getenv(env);
        *counter = (v != NULL) ? (int)strtol(v, NULL, 10) : 0;
    }
    if (*counter > 0) { (*counter)--; return 1; }
    return 0;
}
static int g_fault_open = -2, g_fault_close = -2;

int
stopless_revoke_open(void)
{
    if (fault_budget("STOPLESS_FAULT_OPEN", &g_fault_open)) {
        fprintf(stderr, "[stopless] FAULT-INJECT: revoke_open forced failure\n");
        return -1;
    }
    if (ensure_cri() != 0) return -1;
    atomic_thread_fence(memory_order_acq_rel);
    cheri_revoke_epoch_t e0 = g_cri->epochs.enqueue;
    int rc = cheri_revoke(CHERI_REVOKE_IGNORE_START, e0, NULL);
    if (rv_verbose()) {
        fprintf(stderr, "[stopless] revoke_open rc=%d errno=%d enq=%llu deq=%llu\n",
                rc, errno,
                (unsigned long long)g_cri->epochs.enqueue,
                (unsigned long long)g_cri->epochs.dequeue);
        fflush(stderr);
    }
    /* The opening pass returns non-zero on some kernels even when the epoch
       advanced (stats/copyout quirks). The epoch counter is the truth. */
    return (g_cri->epochs.enqueue > e0 || rc == 0) ? 0 : rc;
}

int
stopless_revoke_close(void)
{
    if (fault_budget("STOPLESS_FAULT_CLOSE", &g_fault_close)) {
        fprintf(stderr, "[stopless] FAULT-INJECT: revoke_close forced failure\n");
        return -1;
    }
    if (ensure_cri() != 0) return -1;
    atomic_thread_fence(memory_order_acq_rel);
    cheri_revoke_epoch_t target = g_cri->epochs.enqueue;
    int rc = cheri_revoke(CHERI_REVOKE_LAST_PASS, target, NULL);
    int loops = 0;
    while (!cheri_revoke_epoch_clears(g_cri->epochs.dequeue, target) && loops < 10) {
        rc = cheri_revoke(CHERI_REVOKE_LAST_PASS, target, NULL);
        loops++;
    }
    if (rv_verbose()) {
        fprintf(stderr, "[stopless] revoke_close rc=%d loops=%d enq=%llu deq=%llu\n",
                rc, loops,
                (unsigned long long)g_cri->epochs.enqueue,
                (unsigned long long)g_cri->epochs.dequeue);
        fflush(stderr);
    }
    return cheri_revoke_epoch_clears(g_cri->epochs.dequeue, target) ? 0 : -1;
}
