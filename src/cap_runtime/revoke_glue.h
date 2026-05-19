// revoke_glue.h — Phase 2 integration with Cornucopia Reloaded's per-page
// capability revocation.
//
// Phase 1 stubs these out; the side_table + forwarding_table from Phase 1
// are sufficient on their own for a working ZGC port. Phase 2 wires this
// up to drive hardware load-barrier traps.
//
// See docs/02_phase_ii_cheri_barrier.md §3 for the protocol with
// CheriBSD's revoke userland.

#pragma once

#include "cap_runtime.h"

#include <atomic>

namespace stopless {

class RevokeDriver {
public:
    RevokeDriver();
    ~RevokeDriver();

    // Phase 2: mark every page covered by `region` as revoked. Mutator
    // cap-loads to revoked pages will trap to the SIGCAPRVOKE handler,
    // which consults the forwarding table to self-heal.
    void revoke_region(stopless_region_t* region);

    // Phase 2: block until Cornucopia reports all revoked pages are
    // healed (no outstanding stale caps).
    void quiesce();

    // SIGCAPRVOKE handler entrypoint. Static so it can be set with
    // sigaction(); Phase 2 fills in the body.
    static void on_sigcaprevoke(int signo, void* info, void* ctx);

private:
    std::atomic<uint64_t> epoch_{0};
};

}  // namespace stopless
