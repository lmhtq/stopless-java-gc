/*
 * forward_table.h — from-cap-address → to-cap mapping for moved objects.
 *
 * Required properties:
 *   - O(1) lookup on the handler hot path
 *   - thread-safe (mutator handler reads, collector writes)
 *   - small memory overhead
 *
 * Implementation: power-of-two open-addressed hash map keyed by
 * the from-cap's *address* (not full cap — caps may have differing
 * bounds for the same object during transitional phases).
 *
 * Concurrency: single writer (collector), many readers (mutator
 * handlers). Writes use store-release; reads use load-acquire. The
 * hash table never shrinks while readers may be active; resize is
 * RCU-style (allocate new, copy, store-release pointer, retire old).
 */

#ifndef STOPLESS_GC_FORWARD_TABLE_H
#define STOPLESS_GC_FORWARD_TABLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Insert: from_addr -> new_cap. NULL new_cap == "delete entry". */
void forward_table_insert(uintptr_t from_addr, void *new_cap);

/* C-9/C-10: multi-writer insert-or-get. Returns the authoritative cap for
   from_addr — new_cap if this caller won the race, else the cap installed
   by whoever won. The single linearization point for concurrent moves. */
void *forward_table_cas_insert(uintptr_t from_addr, void *new_cap);

/* Lookup: returns the to-cap or NULL if not present. */
void *forward_table_lookup(uintptr_t from_addr);

/* Diagnostics. */
size_t forward_table_size(void);
void   forward_table_init(void);
void   forward_table_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* STOPLESS_GC_FORWARD_TABLE_H */
