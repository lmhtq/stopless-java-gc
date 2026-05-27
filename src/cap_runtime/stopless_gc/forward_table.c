/*
 * forward_table.c — simple open-addressed hash map for forwarding.
 *
 * V1: single global table, fixed size. Resize / RCU is V2.
 */

#include "forward_table.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define FT_INITIAL_CAP 8192  /* must be power of 2 */

typedef struct ft_slot {
    _Atomic(uintptr_t) from_addr;
    _Atomic(void *)    new_cap;
} ft_slot_t;

static ft_slot_t *g_table = NULL;
static size_t     g_capacity = 0;
static _Atomic size_t g_size = 0;

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
    if (g_table != NULL) return;
    g_table = calloc(FT_INITIAL_CAP, sizeof(ft_slot_t));
    g_capacity = FT_INITIAL_CAP;
    atomic_store(&g_size, 0);
}

void
forward_table_clear(void)
{
    if (g_table == NULL) return;
    memset(g_table, 0, g_capacity * sizeof(ft_slot_t));
    atomic_store(&g_size, 0);
}

void
forward_table_insert(uintptr_t from_addr, void *new_cap)
{
    if (g_table == NULL) forward_table_init();
    size_t mask = g_capacity - 1;
    size_t i = mix(from_addr) & mask;
    for (size_t probe = 0; probe < g_capacity; probe++) {
        uintptr_t k = atomic_load_explicit(&g_table[i].from_addr,
                                           memory_order_acquire);
        if (k == 0 || k == from_addr) {
            atomic_store_explicit(&g_table[i].new_cap, new_cap,
                                  memory_order_release);
            atomic_store_explicit(&g_table[i].from_addr, from_addr,
                                  memory_order_release);
            if (k == 0) atomic_fetch_add(&g_size, 1);
            return;
        }
        i = (i + 1) & mask;
    }
    /* Table full — V1 just drops; V2 will resize. */
}

void *
forward_table_lookup(uintptr_t from_addr)
{
    if (g_table == NULL) return NULL;
    size_t mask = g_capacity - 1;
    size_t i = mix(from_addr) & mask;
    for (size_t probe = 0; probe < g_capacity; probe++) {
        uintptr_t k = atomic_load_explicit(&g_table[i].from_addr,
                                           memory_order_acquire);
        if (k == 0) return NULL;
        if (k == from_addr) {
            return atomic_load_explicit(&g_table[i].new_cap,
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
