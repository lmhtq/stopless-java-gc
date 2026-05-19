// revoke_glue.cc — Phase 2 stubs. Body fills in once Phase 0 spike S3
// confirms the Cornucopia revoke API surface.

#include "revoke_glue.h"

#include <cstdio>

namespace stopless {

RevokeDriver::RevokeDriver() = default;
RevokeDriver::~RevokeDriver() = default;

void RevokeDriver::revoke_region(stopless_region_t* /*region*/) {
    // TODO(phase2): call into CheriBSD revoke API:
    //   1. Determine the page range covered by `region`.
    //   2. Invoke cheri_revoke(2) (or equivalent userspace shim) with that
    //      range and the current epoch.
    //   3. Increment epoch_.
    epoch_.fetch_add(1, std::memory_order_relaxed);
}

void RevokeDriver::quiesce() {
    // TODO(phase2): poll Cornucopia's per-page bitmap until all pages from
    // the current epoch are clean. Block-with-backoff to avoid burning a
    // core on the polling loop.
}

void RevokeDriver::on_sigcaprevoke(int /*signo*/,
                                   void* /*info*/,
                                   void* /*ctx*/) {
    // TODO(phase2): extract loaded-cap from siginfo_t, look up forwarding
    // table on the owning heap, write the fresh cap back to the trap site,
    // resume mutator. Per-thread forwarding cache prevents repeated work.
    fprintf(stderr, "[stopless] sigcaprevoke handler invoked (stub)\n");
}

}  // namespace stopless
