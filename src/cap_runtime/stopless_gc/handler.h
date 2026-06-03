/*
 * handler.h — SIGPROT handler installation.
 *
 * On install, all subsequent CHERI tag-zero load/store faults are
 * intercepted. If the faulting cap address is in the forwarding
 * table, the handler rematerializes the cap, optionally self-heals
 * the source slot, and resumes execution (with the target register
 * set to the new cap). Otherwise re-raises the signal (real bug).
 */

#ifndef STOPLESS_GC_HANDLER_H
#define STOPLESS_GC_HANDLER_H

#include <stdint.h>   /* uintptr_t (evacuate callback signature) */

#ifdef __cplusplus
extern "C" {
#endif

int  stopless_handler_install(void);
void stopless_handler_remove(void);

/* C-9 boot-crash diagnostics: re-arm a sigaltstack-based crash dumper that
   overrides HotSpot's signal handlers (so a ret-to-stack boot crash is caught
   with a full trapframe + stack-window + frame-chain dump). Idempotent. Must be
   called from a late hook (after HotSpot signal init). */
void stopless_install_crash_diag(void);

/* C-9/C-10: register the mutator-assisted-evacuation callback. Called when a
   tag-0 cap faults whose address is not yet forwarded; the cb evacuates the
   object (copy + cas_insert) and returns the new cap, or NULL if the address
   is not a live GC object. */
void stopless_set_evacuate_cb(void *(*cb)(uintptr_t from_addr));

/* Diagnostics counters (read by api.h::stopless_stats_read). */
extern unsigned long long stopless_handler_faults;
extern unsigned long long stopless_handler_self_heals;
extern unsigned long long stopless_handler_assists;

#ifdef __cplusplus
}
#endif

#endif /* STOPLESS_GC_HANDLER_H */
