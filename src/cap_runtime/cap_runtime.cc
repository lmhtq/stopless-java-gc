// cap_runtime.cc — heap lifecycle + diagnostics + ABI version.
// Bulk of the per-feature work lives in side_table.cc, forwarding_table.cc,
// and revoke_glue.cc.

#include "cap_runtime.h"
#include "side_table.h"
#include "forwarding_table.h"
#include "revoke_glue.h"

#include <cstdio>
#include <cstdlib>
#include <new>

// Internal heap layout. Opaque to callers; defined here so the per-feature
// modules can reach into it via friend declarations in their headers.
struct stopless_heap_s {
    size_t heap_bytes;
    stopless::SideTable        side_table;
    stopless::ForwardingTable  forwarding;
    stopless::RevokeDriver     revoke;

    stopless_heap_s(size_t bytes)
        : heap_bytes(bytes),
          side_table(bytes),
          forwarding(bytes),
          revoke()
    {}
};

extern "C" {

stopless_heap_t* stopless_heap_create(size_t heap_bytes) {
    if (heap_bytes == 0) return nullptr;
    return new (std::nothrow) stopless_heap_t(heap_bytes);
}

void stopless_heap_destroy(stopless_heap_t* heap) {
    delete heap;
}

uint32_t stopless_cap_runtime_abi_version(void) {
    return STOPLESS_CAP_RUNTIME_ABI_VERSION;
}

stopless_color_t stopless_side_table_load_color(stopless_heap_t* heap,
                                                stopless_cap_t   cap) {
    return heap->side_table.load(cap);
}

void stopless_side_table_store_color(stopless_heap_t* heap,
                                     stopless_cap_t   cap,
                                     stopless_color_t color) {
    heap->side_table.store(cap, color);
}

void stopless_side_table_flip_region(stopless_heap_t*  heap,
                                     stopless_region_t* region,
                                     stopless_color_t   from,
                                     stopless_color_t   to) {
    heap->side_table.flip_region(region, from, to);
}

void stopless_forwarding_install(stopless_heap_t* heap,
                                 stopless_cap_t   from,
                                 stopless_cap_t   to) {
    heap->forwarding.install(from, to);
}

stopless_cap_t stopless_forwarding_lookup(stopless_heap_t* heap,
                                          stopless_cap_t   from) {
    return heap->forwarding.lookup(from);
}

void stopless_forwarding_clear_region(stopless_heap_t*  heap,
                                      stopless_region_t* region) {
    heap->forwarding.clear_region(region);
}

void stopless_revoke_region(stopless_heap_t*  heap,
                            stopless_region_t* region) {
    heap->revoke.revoke_region(region);
}

void stopless_revoke_quiesce(stopless_heap_t* heap) {
    heap->revoke.quiesce();
}

void stopless_dump_stats(stopless_heap_t* heap) {
    if (!heap) {
        fprintf(stderr, "[stopless] heap=NULL\n");
        return;
    }
    fprintf(stderr,
            "[stopless] heap=%zu bytes  abi=%u  side_table=%zu installed=%zu\n",
            heap->heap_bytes,
            stopless_cap_runtime_abi_version(),
            heap->side_table.entry_count(),
            heap->forwarding.installed_count());
}

}  // extern "C"
