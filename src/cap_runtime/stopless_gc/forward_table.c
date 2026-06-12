/*
 * forward_table.c — open-addressed hash map for forwarding.
 *
 * C-10 KEY DESIGN POINT: the table stores forwarding *addresses + lengths*
 * (integers), NOT capabilities to the moved objects. Storing a real heap cap
 * is fatal under a CONCURRENT collector: when an object is moved AGAIN in a
 * later cycle and its (now-old) location is revoked, the kernel cheri_revoke
 * sweep scans ALL memory and clears the tag of any cap pointing into the
 * revoked range — INCLUDING the forward-table entry that still points at
 * that intermediate address. The entry then reads back untagged and a
 * mutator gets an untagged oop -> fatal unforwarded fault. (Confirmed:
 * STOPLESS_NO_REVOKE makes the corruption vanish entirely.)
 *
 * Fix: store the destination ADDRESS + LENGTH (integers, immune to
 * revocation) and rebuild the oop capability at lookup time from the arena
 * base cap (which carries PERM_SW_VMEM and is itself revocation-immune):
 *     cap = bounds_set(address_set(arena_base, addr), len)  & ~SW_VMEM
 * — the exact same derivation the allocator used, so the rebuilt cap has
 * the SAME TIGHT BOUNDS as the original. Tight bounds are load-bearing:
 * the interior-pointer heal and the acmp identity barrier both key their
 * forward-table lookups on cheri_base(cap) == object start. (The first
 * address-mode attempt rebuilt ARENA-WIDE caps and broke exactly that:
 * acmp normalize missed, identity broke, MethodHandles' identity-keyed
 * caches went insane -> NULL receivers.)
 *
 * Concurrency: single writer (collector at a safepoint), many readers
 * (mutator handlers). Slot publish order: len, then addr, then key; readers
 * key off from_addr (acquire) and spin on new_addr==0 in cas_insert.
 * Grows at >70% load; the old table is leaked (a reader may be mid-lookup
 * on it) — bounded by geometric growth.
 */

#include "forward_table.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <cheriintrin.h>
#include <cheri/cherireg.h>

#define FT_INITIAL_CAP (1u<<16)  /* must be power of 2; grows on demand */

typedef struct ft_slot {
    _Atomic(uintptr_t) from_addr;
    _Atomic(uintptr_t) new_addr;   /* integer: immune to revocation */
    _Atomic(size_t)    new_len;    /* object length for tight-bounds rebuild */
} ft_slot_t;

static _Atomic(ft_slot_t *) g_table = NULL;
static _Atomic size_t       g_capacity = 0;
static _Atomic size_t       g_size = 0;

/* Arena base cap (carries PERM_SW_VMEM, revocation-immune). Set once at heap
   init before any collect; never changes. */
static void *g_arena_base = NULL;

void
forward_table_set_arena_base(void *base)
{
    g_arena_base = base;
}

/* Rebuild the oop cap for a moved object: identical derivation to the
   allocator (tight bounds, SW_VMEM stripped). */
static unsigned long long g_ft_derive_fail = 0;
static inline void *
ft_derive(uintptr_t addr, size_t len)
{
    if (addr == 0 || g_arena_base == NULL) return NULL;
    void *c = cheri_address_set(g_arena_base, addr);
    if (len > 0) c = cheri_bounds_set(c, len);
    c = cheri_perms_and(c, ~CHERI_PERM_SW_VMEM);
    if (!cheri_tag_get(c) || (uintptr_t)cheri_address_get(c) != addr) {
        if (g_ft_derive_fail < 4) {
            fprintf(stderr, "[ft-derive] FAIL addr=%#lx len=%zu -> tag=%d got=%#lx\n",
                    (unsigned long)addr, len, (int)cheri_tag_get(c),
                    (unsigned long)cheri_address_get(c));
            fflush(stderr);
        }
        g_ft_derive_fail++;
    }
    return c;
}

static inline uintptr_t
mix(uintptr_t x)
{
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
    if (nt == NULL) return;
    size_t mask = newcap - 1;
    for (size_t j = 0; j < cap; j++) {
        uintptr_t k = atomic_load_explicit(&old[j].from_addr, memory_order_relaxed);
        if (k == 0) continue;
        uintptr_t v = atomic_load_explicit(&old[j].new_addr, memory_order_relaxed);
        size_t    l = atomic_load_explicit(&old[j].new_len, memory_order_relaxed);
        size_t i = mix(k) & mask;
        while (atomic_load_explicit(&nt[i].from_addr, memory_order_relaxed) != 0) {
            i = (i + 1) & mask;
        }
        atomic_store_explicit(&nt[i].new_len, l, memory_order_relaxed);
        atomic_store_explicit(&nt[i].new_addr, v, memory_order_relaxed);
        atomic_store_explicit(&nt[i].from_addr, k, memory_order_relaxed);
    }
    atomic_store_explicit(&g_capacity, newcap, memory_order_release);
    atomic_store_explicit(&g_table, nt, memory_order_release);
    /* old table deliberately leaked — see file header. */
}

void
forward_table_insert(uintptr_t from_addr, void *new_cap)
{
    ft_slot_t *t = atomic_load_explicit(&g_table, memory_order_acquire);
    if (t == NULL) { forward_table_init(); t = atomic_load_explicit(&g_table, memory_order_acquire); }
    uintptr_t na = (uintptr_t)cheri_address_get(new_cap);
    size_t    nl = (size_t)cheri_length_get(new_cap);
    size_t mask = atomic_load_explicit(&g_capacity, memory_order_acquire) - 1;
    size_t i = mix(from_addr) & mask;
    for (size_t probe = 0; probe <= mask; probe++) {
        uintptr_t k = atomic_load_explicit(&t[i].from_addr, memory_order_acquire);
        if (k == 0 || k == from_addr) {
            atomic_store_explicit(&t[i].new_len, nl, memory_order_release);
            atomic_store_explicit(&t[i].new_addr, na, memory_order_release);
            atomic_store_explicit(&t[i].from_addr, from_addr, memory_order_release);
            if (k == 0) atomic_fetch_add(&g_size, 1);
            return;
        }
        i = (i + 1) & mask;
    }
}

void *
forward_table_cas_insert(uintptr_t from_addr, void *new_cap)
{
    ft_slot_t *t = atomic_load_explicit(&g_table, memory_order_acquire);
    if (t == NULL) { forward_table_init(); t = atomic_load_explicit(&g_table, memory_order_acquire); }
    uintptr_t na = (uintptr_t)cheri_address_get(new_cap);
    size_t    nl = (size_t)cheri_length_get(new_cap);
    size_t mask = atomic_load_explicit(&g_capacity, memory_order_acquire) - 1;
    size_t i = mix(from_addr) & mask;
    for (size_t probe = 0; probe <= mask; probe++) {
        uintptr_t k = atomic_load_explicit(&t[i].from_addr, memory_order_acquire);
        if (k == from_addr) {
            uintptr_t w;
            while ((w = atomic_load_explicit(&t[i].new_addr, memory_order_acquire)) == 0) {}
            return forward_table_lookup(from_addr);   /* transitive chase */
        }
        if (k == 0) {
            uintptr_t expected = 0;
            if (atomic_compare_exchange_strong_explicit(
                    &t[i].from_addr, &expected, from_addr,
                    memory_order_acq_rel, memory_order_acquire)) {
                atomic_store_explicit(&t[i].new_len, nl, memory_order_release);
                atomic_store_explicit(&t[i].new_addr, na, memory_order_release);
                atomic_fetch_add(&g_size, 1);
                return ft_derive(na, nl);
            }
            if (expected == from_addr) {
                uintptr_t w;
                while ((w = atomic_load_explicit(&t[i].new_addr, memory_order_acquire)) == 0) {}
                return forward_table_lookup(from_addr);   /* transitive chase */
            }
        }
        i = (i + 1) & mask;
    }
    return NULL;
}

/* Raw one-hop probe: *na/*nl <- mapping for from_addr. Returns 1 on hit. */
static int
ft_find(uintptr_t from_addr, uintptr_t *na, size_t *nl)
{
    ft_slot_t *t = atomic_load_explicit(&g_table, memory_order_acquire);
    if (t == NULL) return 0;
    size_t mask = atomic_load_explicit(&g_capacity, memory_order_acquire) - 1;
    size_t i = mix(from_addr) & mask;
    for (size_t probe = 0; probe <= mask; probe++) {
        uintptr_t k = atomic_load_explicit(&t[i].from_addr, memory_order_acquire);
        if (k == 0) return 0;
        if (k == from_addr) {
            uintptr_t a = atomic_load_explicit(&t[i].new_addr, memory_order_acquire);
            if (a == 0) return 0;       /* slot mid-publish: treat as absent */
            *na = a;
            *nl = atomic_load_explicit(&t[i].new_len, memory_order_acquire);
            return 1;
        }
        i = (i + 1) & mask;
    }
    return 0;
}

/* Lookup is TRANSITIVE: chase A -> A' -> A'' inside the integer table and
   derive a cap only for the FINAL address. One hop is NOT enough: if A' was
   itself moved+revoked in a later cycle, a cap to A' freshly derived here
   (post-sweep, from the revocation-exempt arena root) would be VALID and the
   mutator would silently read the stale A' copy -- revocation only kills
   caps that exist at sweep time, it does not poison the address range. So
   the expected "second fault that heals A'->A''" never happens unless the
   first heal raced between the two sweeps. (Found via external review of
   the paper's incorrect one-fault-per-generation claim, 2026-06-12.)
   Depth-bounded: the table maps distinct from-addresses and the arena never
   re-issues addresses (bump alloc, no reuse), so chains are acyclic;
   FT_CHASE_MAX is paranoia against table corruption. */
#define FT_CHASE_MAX 128
void *
forward_table_lookup(uintptr_t from_addr)
{
    uintptr_t na; size_t nl;
    if (!ft_find(from_addr, &na, &nl)) return NULL;
    for (int depth = 0; depth < FT_CHASE_MAX; depth++) {
        uintptr_t nna; size_t nnl;
        if (!ft_find(na, &nna, &nnl)) break;
        if (nna == na) break;          /* self-map paranoia */
        na = nna; nl = nnl;
    }
    return ft_derive(na, nl);
}

/* Address-based storage cannot be tag-corrupted; report only a missing
   arena base. Kept for the C9_FT_AUDIT harness. */
size_t
forward_table_audit(void)
{
    if (g_arena_base == NULL) {
        fprintf(stderr, "[ft-audit] arena base not set!\n");
        return 1;
    }
    return 0;
}

size_t
forward_table_size(void)
{
    return atomic_load(&g_size);
}
