// cap_runtime.h — public C-compatible ABI for the stopless cap_runtime.
//
// This is the only header HotSpot includes. It is intentionally narrow:
// every callable is `extern "C"`, every type is either an opaque struct
// or a small POD. The intent is that hooking this into HotSpot is a
// single small patch (`patches/openjdk-jdk17/0001-cap-runtime-hook.patch`)
// and that all CHERI / GC bookkeeping logic lives in this library, not
// scattered through HotSpot.
//
// Build flavor:
//   STOPLESS_PURECAP=1 → built with Morello clang, capabilities are real
//   STOPLESS_PURECAP=0 → host build for unit tests; capabilities are 64b ptrs
//
// See docs/01_phase_i_zgc_port.md §3 for design rationale.

#pragma once

#include <cstddef>
#include <cstdint>

#ifndef STOPLESS_PURECAP
#define STOPLESS_PURECAP 0
#endif

#if STOPLESS_PURECAP
// On Morello, references are 128-bit capabilities. We expose them through
// the void* __capability type which Morello clang understands. Host builds
// fall back to plain void* so that unit tests can run on x86 and aarch64.
typedef void* __capability stopless_cap_t;
#else
typedef void* stopless_cap_t;
#endif

// Opaque types
struct stopless_heap_s;
typedef struct stopless_heap_s stopless_heap_t;

struct stopless_region_s;
typedef struct stopless_region_s stopless_region_t;

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// Heap lifecycle
// -----------------------------------------------------------------------------

// Initialize a stopless heap of approximately `heap_bytes`. The cap_runtime
// owns the heap-internal bookkeeping (side-table, forwarding tables). The
// actual Java heap memory continues to be managed by ZGC (we cooperate via
// callbacks; see ZGC patches).
stopless_heap_t* stopless_heap_create(size_t heap_bytes);

// Destroy a heap and release bookkeeping memory. Idempotent w.r.t. NULL.
void stopless_heap_destroy(stopless_heap_t* heap);

// Convenience: ABI version of this library, baked in at compile time.
// HotSpot patches can assert against this to detect mismatched builds.
uint32_t stopless_cap_runtime_abi_version(void);

// -----------------------------------------------------------------------------
// Side-table — Phase 1 color storage (replaces ZGC colored-pointer high bits)
// -----------------------------------------------------------------------------

// Color values. Mirrors ZGC's marked0/marked1/remapped/finalizable.
typedef enum {
    STOPLESS_COLOR_GOOD        = 0,
    STOPLESS_COLOR_MARKED0     = 1,
    STOPLESS_COLOR_MARKED1     = 2,
    STOPLESS_COLOR_REMAPPED    = 3,
    STOPLESS_COLOR_FINALIZABLE = 4,
} stopless_color_t;

// O(1) load/store of color for the object pointed to by `cap`. Implementation
// uses a per-card-size shadow bitmap (one byte per object header alignment).
stopless_color_t stopless_side_table_load_color(stopless_heap_t* heap,
                                                stopless_cap_t   cap);
void             stopless_side_table_store_color(stopless_heap_t* heap,
                                                 stopless_cap_t   cap,
                                                 stopless_color_t color);

// Bulk color-flip across all objects in a region. Called at GC phase
// boundaries (e.g., transition from mark0 → mark1).
void stopless_side_table_flip_region(stopless_heap_t*  heap,
                                     stopless_region_t* region,
                                     stopless_color_t   from,
                                     stopless_color_t   to);

// -----------------------------------------------------------------------------
// Forwarding table — Phase 1 cap-aware self-healing
// -----------------------------------------------------------------------------

// Install a forwarding: any subsequent lookup of `from` returns `to`. The
// `to` cap should already have bounds set to the destination region.
void stopless_forwarding_install(stopless_heap_t* heap,
                                 stopless_cap_t   from,
                                 stopless_cap_t   to);

// Look up the forwarding for `from`. If no forwarding is installed, returns
// `from` unchanged (caller's responsibility to know whether self-healing was
// needed). Lookups are O(1) average via a per-region open-addressed hash.
stopless_cap_t stopless_forwarding_lookup(stopless_heap_t* heap,
                                          stopless_cap_t   from);

// Tear down all forwardings for a region. Called after the region's
// evacuation phase completes and the side-table flip has retired every
// stale pointer.
void stopless_forwarding_clear_region(stopless_heap_t*  heap,
                                      stopless_region_t* region);

// -----------------------------------------------------------------------------
// Revoke glue — Phase 2 hardware load-barrier coordination
// -----------------------------------------------------------------------------

// Mark every page of `region` as "containing revoked caps." After this call,
// any mutator cap-load into the region will trap to the SIGCAPRVOKE handler
// installed in revoke_glue.cc.
void stopless_revoke_region(stopless_heap_t*  heap,
                            stopless_region_t* region);

// Block until every page revoked by the current epoch has been observed
// healed (i.e., the Cornucopia bitmap reports clean). Drives the GC's
// "evacuation done" transition.
void stopless_revoke_quiesce(stopless_heap_t* heap);

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------

// Dump a human-readable summary to stderr. Honors JVM verbose-GC flags via
// HotSpot side; this function unconditionally prints.
void stopless_dump_stats(stopless_heap_t* heap);

#ifdef __cplusplus
}  // extern "C"
#endif

// ABI version. Bump only when the layout of any opaque type changes or
// when a function signature changes. Patches that assume an older ABI
// fail to link; callers can also branch on this at runtime.
#define STOPLESS_CAP_RUNTIME_ABI_VERSION 1u
