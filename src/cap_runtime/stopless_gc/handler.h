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

#ifdef __cplusplus
extern "C" {
#endif

int  stopless_handler_install(void);
void stopless_handler_remove(void);

/* Diagnostics counters (read by api.h::stopless_stats_read). */
extern unsigned long long stopless_handler_faults;
extern unsigned long long stopless_handler_self_heals;

#ifdef __cplusplus
}
#endif

#endif /* STOPLESS_GC_HANDLER_H */
