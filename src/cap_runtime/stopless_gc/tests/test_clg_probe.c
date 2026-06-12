/*
 * test_clg_probe.c — feasibility probe for LOAD-SIDE (CLG) revocation under
 * QEMU-Morello. Decides whether the Cornucopia-Reloaded-style per-page
 * capability-load generation barrier is usable for Phase-2 StoplessGC.
 *
 * Questions answered empirically:
 *   Q1. Does cheri_revoke(0)  (OPENING pass, no LAST_PASS) return quickly
 *       (no full data scan) and advance the epoch machinery?
 *   Q2. After the opening pass, does a CAPABILITY LOAD from a not-yet-swept
 *       page observe the revocation (i.e., the CLG fault handler lazily
 *       scans the page and the loaded cap comes back untagged)?
 *   Q3. Do the kernel stats report CLG fault handler activity
 *       (fault_visits > 0)?
 *   Q4. How does CHERI_REVOKE_ASYNC behave (worker-thread scan)?
 *
 * If Q2 is YES, the load-side barrier works under QEMU and the batching
 * consistency window can be closed Cornucopia-Reloaded-style.
 */

#include "../revoke.h"
#include "../forward_table.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <cheri/revoke.h>
#include <cheri/libcaprevoke.h>
#include <cheriintrin.h>
#include <cheri/cherireg.h>

static double
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static const struct cheri_revoke_info *cri;

static void
print_stats(const char *tag, const struct cheri_revoke_stats *st)
{
    printf("  [%s] pages_scan_ro=%u rw=%u faulted_ro=%u rw=%u "
           "CLG_fault_visits=%u caps_found=%u caps_cleared=%u\n",
           tag, st->pages_scan_ro, st->pages_scan_rw,
           st->pages_faulted_ro, st->pages_faulted_rw,
           st->fault_visits, st->caps_found, st->caps_cleared);
}

int
main(void)
{
    /* --- setup: arena + object + a separate slot page holding a cap --- */
    stopless_arena_t a;
    if (stopless_arena_init(&a, 4u << 20) != 0) { printf("FAIL arena\n"); return 1; }

    void *p_info = NULL;
    if (cheri_revoke_get_shadow(CHERI_REVOKE_SHADOW_INFO_STRUCT, NULL, &p_info) != 0) {
        printf("FAIL info struct\n"); return 1;
    }
    cri = (const struct cheri_revoke_info *)p_info;

    const size_t obj_size = 256;
    char *obj = (char *)cheri_bounds_set((char *)a.base, obj_size);
    obj = (char *)cheri_perms_and(obj, ~CHERI_PERM_SW_VMEM);
    memset(obj, 0x5A, obj_size);

    /* Slot page: ordinary anonymous cap-bearing memory, NOT in the arena, so
       the sweep must visit it to kill the stored cap. Many copies across
       several pages so the lazy path has real work. */
    const size_t slot_bytes = 16u << 20;   /* 16 MiB of cap-dense pages */
    void * volatile *slots = mmap(NULL, slot_bytes,
                                  PROT_READ | PROT_WRITE | PROT_CAP |
                                  PROT_MAX(PROT_READ | PROT_WRITE | PROT_CAP),
                                  MAP_ANON | MAP_PRIVATE, -1, 0);
    if (slots == MAP_FAILED) { printf("FAIL slots mmap\n"); return 1; }
    size_t nslots = slot_bytes / sizeof(void *);
    for (size_t i = 0; i < nslots; i++) slots[i] = obj;
    printf("setup: %zu cap slots across %zu pages, all -> obj\n",
           nslots, slot_bytes / 4096);

    /* mark the object for revocation */
    stopless_mark_revoke(&a, (uintptr_t)cheri_address_get(obj), obj_size);

    cheri_revoke_epoch_t e0 = cri->epochs.enqueue;
    struct cheri_revoke_stats st; memset(&st, 0, sizeof(st));

    /* --- Q1: opening pass (NO last-pass) --- */
    double t0 = now_ms();
    int rc = cheri_revoke(CHERI_REVOKE_IGNORE_START, e0, &st);
    double t_open = now_ms() - t0;
    printf("Q1 opening pass: rc=%d errno=%d t=%.1f ms epochs enq=%lu deq=%lu\n",
           rc, errno, t_open,
           (unsigned long)cri->epochs.enqueue, (unsigned long)cri->epochs.dequeue);
    print_stats("open", &st);

    /* --- Q2: capability load from (possibly) unswept pages --- */
    /* Probe slots spread across the whole range: if load-side is active,
       these loads CLG-fault, the kernel scans the page, and the loaded cap
       is untagged. If only store-side/sync exists, the opening pass already
       scanned everything (Q1 time will show it) or tags survive. */
    int untagged = 0, tagged = 0;
    double t1 = now_ms();
    for (size_t i = 0; i < nslots; i += nslots / 64) {
        void *c = slots[i];
        if (cheri_tag_get(c)) tagged++; else untagged++;
    }
    double t_probe = now_ms() - t1;
    printf("Q2 probe loads (64 spread): tagged=%d untagged=%d t=%.1f ms\n",
           tagged, untagged, t_probe);

    /* --- close the epoch --- */
    memset(&st, 0, sizeof(st));
    t0 = now_ms();
    rc = cheri_revoke(CHERI_REVOKE_LAST_PASS, cri->epochs.enqueue, &st);
    double t_close = now_ms() - t0;
    printf("close (LAST_PASS): rc=%d t=%.1f ms epochs enq=%lu deq=%lu\n",
           rc, t_close,
           (unsigned long)cri->epochs.enqueue, (unsigned long)cri->epochs.dequeue);
    print_stats("close", &st);

    int post_tagged = 0;
    for (size_t i = 0; i < nslots; i += nslots / 64)
        if (cheri_tag_get((void *)slots[i])) post_tagged++;
    printf("post-close probe: tagged=%d (expect 0)\n", post_tagged);

    /* --- Q4: ASYNC pass on a fresh object --- */
    char *obj2 = (char *)cheri_bounds_set((char *)a.base + 4096, obj_size);
    obj2 = (char *)cheri_perms_and(obj2, ~CHERI_PERM_SW_VMEM);
    memset(obj2, 0x6B, obj_size);
    for (size_t i = 0; i < nslots; i++) slots[i] = obj2;
    stopless_mark_revoke(&a, (uintptr_t)cheri_address_get(obj2), obj_size);

    memset(&st, 0, sizeof(st));
    t0 = now_ms();
    rc = cheri_revoke(CHERI_REVOKE_IGNORE_START | CHERI_REVOKE_ASYNC |
                      CHERI_REVOKE_LAST_PASS, cri->epochs.enqueue, &st);
    double t_async = now_ms() - t0;
    printf("Q4 ASYNC|LAST: rc=%d errno=%d t=%.1f ms (call-return latency)\n",
           rc, errno, t_async);
    /* poll the slot: how long until a LOAD sees the revocation? */
    double t_kill = -1; t0 = now_ms();
    for (int spin = 0; spin < 200000; spin++) {
        if (!cheri_tag_get((void *)slots[nslots / 2])) { t_kill = now_ms() - t0; break; }
    }
    printf("Q4 stale cap observed dead after %.1f ms of load-polling "
           "(-1 = still alive)\n", t_kill);
    printf("epochs final: enq=%lu deq=%lu\n",
           (unsigned long)cri->epochs.enqueue, (unsigned long)cri->epochs.dequeue);

    printf("CLG-PROBE-DONE\n");
    return 0;
}
