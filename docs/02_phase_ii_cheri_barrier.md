# Phase 2 — CHERI-native ZGC Barrier

## 1. Goal

Replace ZGC's software load barrier — currently an `AND` + `CBNZ` against a
"bad mask" embedded in each oop — with a **CHERI cap-load whose
hardware-checked tag bit subsumes the bad-mask check**. The result: the
fast path of every reference load is a normal Morello `LDR Cx`. The slow
path (cap tag cleared by Cornucopia Reloaded's revocation primitive)
traps via SIGCAPRVOKE to a software handler that consults Phase 1's
forwarding table and returns a fresh cap.

Phase 1 produced everything except the barrier substitution and the
revocation glue. Phase 2 plugs them in.

## 2. The core swap

### 2.1 Before (Phase 1, ZGC port semantics)

```c++
// Fast path emitted by C2 for each reference load:
oop o = *p;                       // raw load (LDR Cx)
if (o.address() & bad_mask) {     // software color check
    o = barrier_slow(o);           // forwarding / mark / etc.
    *p = o;                        // self-heal
}
return o;
```

The `bad_mask` is updated by the GC at each phase transition. Every load
pays the AND + branch.

### 2.2 After (Phase 2)

```c++
// Fast path:
oop o = *p;                       // LDR Cx — hardware traps if revoked
return o;

// Trap handler (slow path), entered only when revocation cleared the cap tag:
on_sigcaprevoke(siginfo_t *si) {
    oop stale = si->si_addr_as_cap;
    oop fresh = forwarding_table_lookup(stale);
    *si->si_loaded_from = fresh;   // self-heal
    resume_with_value(fresh);
}
```

The bad-mask AND disappears. The branch disappears. The CPU never
executes a check on the fast path — the cap's tag bit is checked by the
load microarchitecture for free.

## 3. Revocation glue

### 3.1 Cornucopia Reloaded mechanism

Cornucopia Reloaded maintains a per-page bitmap indicating which pages
contain capabilities that have been revoked. On every capability load,
the Morello LSU consults the page's revocation bit. If the bit is set
and the loaded cap is in the revoked region, the cap tag is cleared in
the destination register, and on its first use a trap is raised.

### 3.2 What ZGC needs to drive Cornucopia

When ZGC begins relocating a region:

1. Mark every page of the from-space region as "containing revoked caps."
2. Mutator load barriers automatically trap on any stale cap from now on.
3. The trap handler does the forwarding-table lookup.
4. When all from-space caps are healed (no more traps for some interval),
   unmark the pages, free the physical memory.

The unmarking step requires a quiescence detection mechanism. ZGC's
existing color-flip serves this role today; on CHERI we replace it with
"all-pages-clean" status from Cornucopia's bitmap. Implementation:
piggyback on Cornucopia Reloaded's existing epoch counter (Filardo
et al. §4.3).

### 3.3 The forwarding table on the trap path

Phase 1 builds the forwarding table. Phase 2 only adds:

- A per-thread cache of recently-resolved forwarding entries (sized to
  L1: ~64 entries, LRU). Microbenchmarks suggest >90% of trap handlers
  hit the cache; this kills the cost of repeated lookups during
  concurrent evacuation.
- Adaptive trap-handler invocation strategy: the first N traps per
  region prime the cache; afterward we batch-prefetch forwarding entries
  for the rest of the region.

### 3.4 Trap entry cost

Morello FVP measurements (preliminary, from CTSRD-CHERI docs): a
SIGCAPRVOKE round-trip costs roughly 300–600 cycles. Each cap is healed
on first stale load and never re-traps. For a workload that touches N
distinct stale caps during a GC cycle:

```
phase 2 cost  ≈ N × ~400 cycles  (one-time, amortized over cap reuses)
phase 1 cost  ≈ M × ~6 cycles    (every load, M = total reference loads)
```

For workloads where total reference loads ≫ distinct stale caps (most
real workloads — references are loaded many times), Phase 2 should win
clearly.

## 4. Generational consideration

Java 25's generational ZGC splits the heap into young and old. Each
generation has its own color set. On CHERI:

- Young generation: relocation is frequent; CHERI hardware barrier
  shines.
- Old generation: relocation is rare; software barrier was already
  near-free on the steady-state path. Less gain.

**Decision (tentative):** Target non-generational ZGC for Phase 2's
main result. Add a `--enable-generational` build option that compiles
in the dual-color machinery but is not required for the headline
numbers. Generational support is a Phase 3 follow-up if Phase 2
empirically lands.

## 5. Falsifiable claim & measurement

The paper's headline claim:

> On Morello FVP, replacing ZGC's software load barrier with a CHERI
> cap-load barrier reduces per-reference-load CPU cost by ≥3× on
> pointer-chasing benchmarks (DaCapo `h2`, `pmd`; Renaissance
> `scrabble`, `philosophers`), with no statistically significant
> regression in p99.9 GC pause time vs. the Phase 1 port (the
> matched baseline).

We measure:

- **Per-load barrier cost** via PMU counters: dedicated GC barrier
  events on Morello, plus IPC and cycles-in-barrier-slot via
  perf-counter sampling.
- **GC pause time distribution** via JFR + a custom probe that records
  per-pause durations.
- **Throughput** via DaCapo's existing time-to-completion metric.
- **Cap-trap frequency** as a sanity check on the forwarding cache
  hit rate.

Three baselines:

1. ZGC unpatched on Morello FVP (the Phase 1 port).
2. MOJO G1 on Morello FVP (different GC, same platform — lower bound on
   what CHERI GC perf looks like).
3. ZGC on x86_64 (separate machine; not a direct comparison but anchors
   absolute numbers).

## 6. Module breakdown

| Module | New LOC |
|---|---|
| Cap-load barrier emission in `cpu/morello/gc/z/zBarrierSetAssembler.cpp` | 600–1000 |
| SIGCAPRVOKE handler in `cap_runtime/revoke_glue.cc` | 400–700 |
| Forwarding-cache per-thread | 200–400 |
| Cornucopia Reloaded driver (mark/unmark pages, epoch coordination) | 400–700 |
| (Optional) Generational cap-epoch | 400–700 |
| Measurement infra (PMU sampling, JFR probes, plot scripts) | 600–1000 |
| Microbenchmarks + harness | 500–800 |
| **Total** | **~2.7k–5.3k** |

## 7. Main-paper outline

```
Title: CHERI-native load barriers for ZGC — hardware-checked tags
       replace software color masks in a concurrent moving GC

§1 Introduction
   - Software load barrier cost in modern low-pause GCs
   - CHERI cap-load as a candidate hardware substitute
§2 Background
   - ZGC barrier mechanics
   - Cornucopia Reloaded's per-page revocation
   - The forwarding-on-immutable-caps problem (from Phase 1 paper)
§3 Design
   - The cap-load-as-barrier substitution
   - SIGCAPRVOKE-based slow path
   - Forwarding cache and prefetch
§4 Implementation
   - cpu/morello/gc/z changes
   - cap_runtime/revoke_glue
   - Cornucopia driver integration
§5 Evaluation
   - DaCapo / Renaissance throughput
   - Per-load barrier cost (PMU)
   - p99.9 pause time
   - Generational behavior (preview)
§6 Discussion
   - When CHERI barrier helps and when it doesn't
   - Comparison with hypothetical ISA additions (cforward)
§7 Related work
§8 Conclusion
```

Target: ASPLOS, ISMM, or PLDI; arXiv preprint immediately on submission.
