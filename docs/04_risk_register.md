# Risk Register

## Methodology note

This project is being implemented with substantial LLM (Claude Opus 4.7)
assistance. **The LLM compresses the front half of the research workflow
(literature review, design space exploration, code generation, paper
drafting). It does not compress the back half (building, measuring,
convincing reviewers).** The risks below are the back-half risks, and
they are not made smaller by AI assistance — they are the limits of
what AI assistance can compress.

## Top risks

### R1 — ZGC's pointer encoding is fundamentally incompatible with CHERI

**Severity:** Critical. If true, Phase 1 cannot deliver the headline
result of "a working ZGC on Morello"; the best outcome is a degraded
ZGC variant that loses the colored-pointer / multi-mapping optimization
and effectively becomes Shenandoah-shaped.

**Why this is plausible:** MOJO ported Epsilon, Serial, and G1 — but
not ZGC. The MOJO team has direct contact with both the ZGC and CHERI
worlds. Their public materials do not document the reason for skipping
ZGC, and one plausible reason is "we tried and it didn't make sense."

**Mitigation:**
- Phase 0 spike S2 (3 days): attempt to build ZGC with the MOJO patch
  set and document every failure. The first 10 build errors reveal
  whether the incompatibility is structural or just engineering work.
- If structural: pivot the workshop paper to the *design* of a
  CHERI-friendly ZGC (i.e., what would have to change in ZGC's source
  to make it portable), with a smaller mechanism demo. Still a
  workshop paper.

### R2 — C2 JIT is not Morello-capability-aware

**Severity:** High. Without C2 emitting capability instructions in the
load barrier inline code, every reference load goes through a stub
call (~10× slower than inline). Phase 2's headline performance claim
collapses.

**Why this is plausible:** MOJO's published work focuses on Serial / G1
runs in C1-only or interpreted mode. C2 cap-awareness on Morello is not
documented as production-ready.

**Mitigation:**
- Phase 0 spike S1 (5 days): run MOJO G1 on Morello FVP with C2 enabled
  (`-XX:+UseG1GC -XX:+TieredCompilation`); check whether C2 emits cap
  instructions correctly.
- If C2 is broken: Phase 1 still ships (we use the C1 barrier path),
  but Phase 2's measurement story has to change — we'd have to add
  C2 support ourselves (~2–3k extra LOC), or pivot to demonstrating
  the mechanism without absolute-perf claims, comparing relative
  cycles in cycle-accurate simulation.

### R3 — Cornucopia Reloaded revocation interface is not JVM-friendly

**Severity:** Medium. Cornucopia's revoke userland is built around
`malloc`/`free` semantics for libc heaps. ZGC manages its own heap and
does not use libc malloc for object allocation.

**Why this matters:** If the revoke interface only operates on
malloc-managed pages, we either need to (a) patch Cornucopia's userland
to accept arbitrary VM-managed pages, or (b) modify ZGC's allocator to
route through Cornucopia-managed pages (which adds a layer of
indirection that may regress throughput).

**Mitigation:**
- Phase 0 spike S3 (2 days): read Cornucopia Reloaded's source in
  `third_party/cheribsd/` and confirm the revoke API surface area.
  Identify the cleanest integration point.
- If neither option is clean: a small CheriBSD kernel patch (~500–1500
  LOC) exposes a page-granularity revoke syscall directly. This is
  out-of-scope for a single arXiv paper but worth a separate workshop
  contribution.

### R4 — Real Morello hardware access never materializes

**Severity:** Low for the immediate plan. Morello FVP gives us
cycle-approximate numbers good enough for arXiv preprints. Real-board
numbers would be a nice-to-have, not a gating dependency.

**Mitigation:**
- Cycle-approximate FVP results are explicitly framed as such in the
  papers; we do not claim absolute performance.
- Apply to DSbD Technology Access Programme as a parallel track; if
  granted, real-board numbers add a fourth eval section to Phase 2.

### R5 — Upstream OpenJDK churn breaks the patch set

**Severity:** Low. We pin to OpenJDK 17u at a specific SHA. Periodic
rebases onto newer 17u tags happen at our cadence, not upstream's.

**Mitigation:** `scripts/upstream_pins.env` is the single source of
truth for pinned SHAs. Patch rebase is mechanical with `git rebase`
plus our test suite.

### R6 — Filardo / Cambridge team publishes the same idea first

**Severity:** Medium for paper novelty, low for the engineering effort.
Cornucopia Reloaded's authors are the most likely competitors. They
have access to Morello hardware, MOJO collaborators, and the relevant
infrastructure.

**Why this is plausible:** Cornucopia Reloaded ends with a "future work"
gesture toward moving GCs. A natural follow-up.

**Mitigation:** Publish arXiv preprint #1 as soon as Phase 1 lands —
priority date matters. Avoid waiting for venue acceptance.

## 2-week feasibility spike (Phase 0)

The first two weeks are not Phase 1. They are a spike to retire R1,
R2, R3 enough to commit to the 5–7 month plan.

| Day | Activity | Output |
|---|---|---|
| 1–2 | Set up cheribuild, build Morello SDK, build CheriBSD image | Reproducible environment |
| 3 | Bring up Morello FVP, run `hello.c` purecap binary | FVP working |
| 4–5 | Apply MOJO G1 patch set, build OpenJDK 17u, run G1 on FVP | Baseline reproduced |
| 6 | Re-run with `-XX:+UseG1GC -XX:+TieredCompilation`; inspect C2 disasm | **R2 retired or escalated** |
| 7–9 | Attempt `make ... -XX:+UseZGC`; collect compile errors | **R1 retired or escalated** |
| 10–11 | Read Cornucopia Reloaded source in `third_party/cheribsd/`; sketch revoke API integration | **R3 retired or escalated** |
| 12 | Update `04_risk_register.md` with empirical findings | Risks resolved |
| 13–14 | Go/no-go decision; write spike report; if go, kick off Phase 1 | Decision committed |

**Go criteria:**
- R1: Either ZGC builds with at most ~5k LOC of new patches, OR the
  failure pattern reveals a fundable degraded-ZGC variant.
- R2: C2 emits cap instructions for G1 correctly, OR we have a clear
  path to add C2 cap-awareness in ≤3k LOC.
- R3: Cornucopia revoke API can be driven from ZGC with ≤1k LOC of
  glue, OR a kernel patch ≤1.5k LOC is acceptable.

**No-go criteria:** Any two of R1/R2/R3 require more than the
mitigation budget. Pivot to Path B (G1 + CHERI-native evacuation
barrier) instead; revise design docs accordingly.

## What AI assistance changes about these risks

The LLM can:
- Read and explain the relevant source faster than a human reviewer.
- Generate patch candidates and unit tests at high velocity.
- Write up findings in publication-grade prose immediately.

The LLM cannot:
- Detect a structural incompatibility that hasn't manifested yet in
  code (R1's worst case).
- Run Morello FVP, read PMU counters, or measure cap-trap frequencies
  (R2's resolution).
- Negotiate with Cornucopia authors or apply to DSbD (R4, R6).

In other words: AI assistance is a productivity multiplier on
**implementation work that is well-defined**. The spike's job is to
make sure the work is well-defined. Until the spike completes, the
risks are not "AI-shrinkable."
