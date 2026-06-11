/*
 * forward_table.c — open-addressed hash map for forwarding.
 *
 * Concurrency: single writer (the collector, inside a safepoint), many
 * readers (mutator SIGPROT handlers). The table grows when load gets high.
 * Growth happens on the writer side at a safepoint; the new table is
 * published with store-release on g_table and the OLD table is intentionally
 * NOT freed — a mutator handler interrupted mid-lookup may still hold a
 * pointer to it. Leaking the old tables is bounded (geometric 2x growth, so
 * total retired memory < current table size) and far cheaper than RCU here.
 */

#include "forward_table.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define FT_INITIAL_CAP (1u<<16)  /* must be power of 2; grows on demand */

typedef struct ft_slot {
    _Atomic(uintptr_t) from_addr;
    _Atomic(void *)    new_cap;
} ft_slot_t;

static _Atomic(ft_slot_t *) g_table = NULL;
static _Atomic size_t       g_capacity = 0;
static _Atomic size_t       g_size = 0;

static inline uintptr_t
mix(uintptr_t x)
{
    /* SplitMix64 from sebastiano vigna. */
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

void
forward_table_init(void)
{
    if (atomic_load_explicit(&g_table, memory_order_acquire) != NULL) return;
    ft_slot_t *t = calloc(FT_INITIAL_CAP, sizeof(ft_slot_t));
    atomic_store_explicit(&g_capacity, FT_INITIAL_CAP, memory_order_release);
    atomic_store_explicit(&g_table, t, memory_order_release);
    atomic_store(&g_size, 0);
}

void
forward_table_clear(void)
{
    ft_slot_t *t = atomic_load_explicit(&g_table, memory_order_acquire);
    size_t cap = atomic_load_explicit(&g_capacity, memory_order_acquire);
    if (t == NULL) return;
    memset(t, 0, cap * sizeof(ft_slot_t));
    atomic_store(&g_size, 0);
}

/* C-9/C-10: grow the table when load exceeds ~70%. MUST be called only by the
   collector at a safepoint (single writer, mutator readers quiesced at poll
   points). Without growth, a long-running concurrent collector overflows the
   fixed table after ~10 whole-heap cycles and silently DROPS forwardings —
   the dropped objects' stale caps then never heal (fatal unforwarded fault). */
void
forward_table_maybe_grow(void)
{
    ft_slot_t *old = atomic_load_explicit(&g_table, memory_order_acquire);
    if (old == NULL) { forward_table_init(); return; }
    size_t cap = atomic_load_explicit(&g_capacity, memory_order_acquire);
    size_t sz  = atomic_load(&g_size);
    if (sz * 10 < cap * 7) return;                 /* load < 70% */

    size_t newcap = cap * 2;
    ft_slot_t *nt = calloc(newcap, sizeof(ft_slot_t));
    if (nt == NULL) return;                         /* keep old table */
    size_t mask = newcap - 1;
    for (size_t j = 0; j < cap; j++) {
        uintptr_t k = atomic_load_explicit(&old[j].from_addr,
                                           memory_order_relaxed);
        if (k == 0) continue;
        void *v = atomic_load_explicit(&old[j].new_cap, memory_order_relaxed);
        size_t i = mix(k) & mask;
        while (atomic_load_explicit(&nt[i].from_addr,
                                    memory_order_relaxed) != 0) {
            i = (i + 1) & mask;
        }
        atomic_store_explicit(&nt[i].from_addr, k, memory_order_relaxed);
        atomic_store_explicit(&nt[i].new_cap, v, memory_order_relaxed);
    }
    /* publish: capacity first, then the table pointer (readers load the table
       last with acquire, so they see a capacity >= the table they got). */
    atomic_store_explicit(&g_capacity, newcap, memory_order_release);
    atomic_store_explicit(&g_table, nt, memory_order_release);
    /* deliberately do NOT free(old) — see file header. */
}

void
forward_table_insert(uintptr_t from_addr, void *new_cap)
{
    ft_slot_t *t = atomic_load_explicit(&g_table, memory_order_acquire);
    if (t == NULL) { forward_table_init(); t = atomic_load_explicit(&g_table, memory_order_acquire); }
    size_t mask = atomic_load_explicit(&g_capacity, memory_order_acquire) - 1;
    size_t i = mix(from_addr) & mask;
    for (size_t probe = 0; probe <= mask; probe++) {
        uintptr_t k = atomic_load_explicit(&t[i].from_addr,
                                           memory_order_acquire);
        if (k == 0 || k == from_addr) {
            atomic_store_explicit(&t[i].new_cap, new_cap,
                                  memory_order_release);
            atomic_store_explicit(&t[i].from_addr, from_addr,
                                  memory_order_release);
            if (k == 0) atomic_fetch_add(&g_size, 1);
            return;
        }
        i = (i + 1) & mask;
    }
    /* Table full — should not happen now that the collector grows it. */
}

/* C-9/C-10: multi-writer insert-or-get. Collector and mutators (assisted
   evacuation) race to install a forwarding for the SAME from_addr; exactly
   one wins and its new_cap becomes authoritative. */
void *
forward_table_cas_insert(uintptr_t from_addr, void *new_cap)
{
    ft_slot_t *t = atomic_load_explicit(&g_table, memory_order_acquire);
    if (t == NULL) { forward_table_init(); t = atomic_load_explicit(&g_table, memory_order_acquire); }
    size_t mask = atomic_load_explicit(&g_capacity, memory_order_acquire) - 1;
    size_t i = mix(from_addr) & mask;
    for (size_t probe = 0; probe <= mask; probe++) {
        uintptr_t k = atomic_load_explicit(&t[i].from_addr,
                                           memory_order_acquire);
        if (k == from_addr) {
            void *w;
            while ((w = atomic_load_explicit(&t[i].new_cap,
                                             memory_order_acquire)) == NULL) {}
            return w;
        }
        if (k == 0) {
            uintptr_t expected = 0;
            if (atomic_compare_exchange_strong_explicit(
                    &t[i].from_addr, &expected, from_addr,
                    memory_order_acq_rel, memory_order_acquire)) {
                atomic_store_explicit(&t[i].new_cap, new_cap,
                                      memory_order_release);
                atomic_fetch_add(&g_size, 1);
                return new_cap;
            }
            if (expected == from_addr) {
                void *w;
                while ((w = atomic_load_explicit(&t[i].new_cap,
                                                 memory_order_acquire)) == NULL) {}
                return w;
            }
            /* slot taken by a different key — keep probing */
        }
        i = (i + 1) & mask;
    }
    return NULL;  /* table full */
}

void *
forward_table_lookup(uintptr_t from_addr)
{
    ft_slot_t *t = atomic_load_explicit(&g_table, memory_order_acquire);
    if (t == NULL) return NULL;
    size_t mask = atomic_load_explicit(&g_capacity, memory_order_acquire) - 1;
    size_t i = mix(from_addr) & mask;
    for (size_t probe = 0; probe <= mask; probe++) {
        uintptr_t k = atomic_load_explicit(&t[i].from_addr,
                                           memory_order_acquire);
        if (k == 0) return NULL;
        if (k == from_addr) {
            return atomic_load_explicit(&t[i].new_cap,
                                        memory_order_acquire);
        }
        i = (i + 1) & mask;
    }
    return NULL;
}

size_t
forward_table_size(void)
{
    return atomic_load(&g_size);
}
