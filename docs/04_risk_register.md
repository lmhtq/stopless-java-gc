# Risk Register

## Methodology note

This project is being implemented with substantial LLM (Claude Opus 4.7)
assistance. **The LLM compresses the front half of the research workflow
(literature review, design space exploration, code generation, paper
drafting). It does not compress the back half (building, measuring,
convincing reviewers).** The risks below are the back-half risks, and
they are not made smaller by AI assistance — they are the limits of
what AI assistance can compress.

## Risk status summary (post source-survey, 2026-05-19)

| Risk | Original severity | Post-survey severity | Evidence doc |
|---|---|---|---|
| **R1** ZGC × CHERI pointer encoding | Critical | **High** (downgraded) — mechanical, not structural | `docs/05_zgc_cheri_collision_report.md` |
| **R2** C2 JIT cap-awareness | High | **High** (confirmed) — Phase 1 runs at C1+interp; Phase 2 measurements get a C1 baseline | `docs/07_c2_jit_status.md` |
| **R3** Cornucopia revoke API JVM-friendliness | Medium | **Low** (downgraded) — API is arena-based, no kernel patch needed | `docs/06_cornucopia_api_survey.md` |
| **R4** Real Morello hardware access | Low | Low (unchanged) — FVP is sufficient for arXiv | — |
| **R5** Upstream OpenJDK churn | Low | Low (unchanged) — pinned SHA | — |
| **R6** Cambridge / Manchester scoops us | Medium | Medium (unchanged) — arXiv early, often | — |

**Net effect**: of the three project-defining risks (R1, R2, R3), one
is substantially retired, one is downgraded, one remains binding.
The 5–7 month plan is **go** with a known performance caveat
(C1-tier baselines in evaluation sections).

## Top risks

### R1 — ZGC's pointer encoding is fundamentally incompatible with CHERI

**Severity:** ~~Critical~~ **High (downgraded 2026-05-19)**.

**Why this was plausible:** MOJO ported Epsilon, Serial, and G1 — but
not ZGC. The MOJO team has direct contact with both the ZGC and CHERI
worlds. Their public materials do not document the reason for skipping
ZGC, and one plausible reason was "we tried and it didn't make sense."

**Evidence from source survey (2026-05-19, docs/05):** Direct read of
`src/hotspot/share/gc/z/zAddress.{hpp,inline.hpp}` at OpenJDK 17u
tag `jdk-17.0.13-ga` confirms ZGC's load barrier is
`value & ZAddressBadMask`, with `value: uintptr_t`. The conflict
with CHERI is structural (cap.address bits cannot carry color), but
**mechanical** — every site of `ZAddress::*` calls follows a uniform
pattern that maps to a side-table consult. ~300 call sites, each
following the same template.

**Mitigation (largely already executed):**
- Phase 0 spike S2 is **partially retired**: we know what the build
  errors will look like (mismatched types around uintptr_t↔cap_t in
  ZAddress methods). The spike now needs only to confirm the
  `zGlobals.hpp` bad-mask layout and verify our side-table approach
  compiles.
- If the spike surfaces a *new* structural issue not visible from
  the headers: pivot the workshop paper to the *design* of a
  CHERI-friendly ZGC, with a smaller mechanism demo. Still a
  workshop paper.

### R2 — C2 JIT is not Morello-capability-aware

**Severity:** High (confirmed 2026-05-19).

**Evidence from literature survey (docs/07):** No public port of a
C2-class JIT to CHERI purecap exists. The 2026 CRuby-on-CHERI paper
(Liu et al., *Art Sci Eng Programming* vol. 11.1) explicitly excludes
the JIT compiler from their port. The only published JIT-on-CHERI
precedent is JavaScriptCore — substantially simpler than HotSpot C2.
MOJO does not advertise C2 cap-awareness; their G1/Serial port runs
C1 + interpreter.

**Mitigation (revised):**
- Phase 1 runs at **C1 + interpreter** (proven feasible by MOJO).
  Add `--with-jvm-features=-compiler2` to `scripts/build_jdk.sh` to
  drop C2 from the build.
- Phase 2's headline claim is reframed honestly: "On a JVM running
  C1 + interpreter, replacing ZGC's software load barrier with a
  CHERI cap-load barrier reduces per-load barrier cost by ≥3×."
- Apples-to-apples comparison uses MOJO G1 (also C1-tier) as
  baseline. Absolute performance numbers are weaker than C2 would
  give, but the *delta* — which is what the paper claims — is
  preserved.
- Optional Phase 1.5: a ~2–3 kLOC C2 cap-awareness patch is a
  separate workshop-paper-grade sub-project. Track but do not gate.

### R3 — Cornucopia Reloaded revocation interface is not JVM-friendly

**Severity:** ~~Medium~~ **Low (downgraded 2026-05-19)**.

**Evidence from source survey (docs/06):** Direct read of
`sys/cheri/revoke.h` on CheriBSD main confirms the API is
**arena-based**, not malloc-tied:

```c
int cheri_revoke_get_shadow(int flags,
                            void * __capability arena,
                            void * __capability *shadow);
int cheri_revoke(int flags,
                 cheri_revoke_epoch_t start_epoch,
                 struct cheri_revoke_syscall_info *crsi);
```

The JVM presents its heap (one or more large mmap regions) as an
arena, requests `CHERI_REVOKE_SHADOW_NOVMEM`, gets a cap to the
shadow bitmap, marks pages, and drives revocation. **No kernel
patch is needed.** The earlier 500–1500 LOC kernel-patch mitigation
budget can be deleted.

**Remaining real R3 risk:**
- The shadow bitmap has cap-representability constraints
  ("the call must fail if the resulting capability would not be
  representable"). The Java heap is typically multi-gigabyte and
  page-aligned, so this should be a no-op. Spike day 11 confirms.
- Per-pass revocation cost is documented in `cheri_revoke_stats`
  but not in absolute time. Cornucopia Reloaded's published numbers
  suggest low per-page cost. We measure our own.

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
R2, R3 enough to commit to the 5–7 month plan. **Source survey on
2026-05-19 has pre-filled most of the analytical work; the spike now
compresses from 12 working days to ~5 days of empirical confirmation.**

| Day | Activity | Status | Output |
|---|---|---|---|
| 1–2 | Set up cheribuild, build Morello SDK, build CheriBSD image | **In progress on `bc@hasee`** — bootstrap.sh running | Reproducible environment |
| 3 | Bring up Morello FVP, run `hello.c` purecap binary | Pending | FVP working |
| 4–5 | Apply MOJO G1 patch set, build OpenJDK 17u, run G1 on FVP | Pending | Baseline reproduced |
| 6 | Re-run with `-XX:+UseG1GC -XX:+TieredCompilation`; inspect C2 disasm | Pending (literature suggests C2 will not work) | **R2 confirm/escalate** |
| 7–9 | Attempt `make ... -XX:+UseZGC`; collect compile errors | Pending (source survey predicts mechanical failures around `uintptr_t` ABI) | **R1 confirm/escalate** |
| ~~10–11~~ | ~~Read Cornucopia Reloaded source~~ | **Done (docs/06)** — API is arena-based, retire R3 | ~~Risk doc update~~ |
| 12 | Empirical confirmation of side-table approach via toy build | Pending | Final risk update |
| 13–14 | Go/no-go decision; write spike report; if go, kick off Phase 1 | Pending | Decision committed |

**Updated go criteria:**
- R1: ZGC builds with ≤5k LOC of new patches against the pinned
  OpenJDK 17u SHA. Source survey predicts this is achievable.
- R2: Phase 1 produces a working JVM at C1+interpreter tier. If
  C2 fails (expected), the build system drops C2 cleanly. Phase 2
  evaluation is reframed against the C1 baseline.
- R3: **Retired** — Cornucopia revoke API is JVM-friendly (docs/06).

**No-go criteria:**
- R1 turns out structural (not mechanical) — i.e., the spike finds
  an issue that the source survey missed. Estimated likelihood: low.
- C1 itself doesn't work on Morello purecap — would invalidate MOJO's
  claims of running G1. Estimated likelihood: very low.
- Pivot to Path B (G1 + CHERI-native evacuation barrier) if any
  no-go fires; design docs already enumerate this fallback.

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

## Go/no-go (2026-05-19, pre-empirical)

**Recommendation: GO**, conditional on Day 7–9 spike confirming R1's
mechanical nature.

Reasoning:
1. R3 is retired by source survey (Cornucopia API is arena-based).
2. R2 is confirmed but bounded (C1 baseline is honest and the paper
   delta is preserved).
3. R1 is downgraded to High but the failure surface is now
   characterized; the spike confirms rather than discovers.
4. R6 (Cambridge / Manchester scoops us) demands arXiv-preprint
   priority — the Phase 1 preprint must go up the day spike S2
   confirms feasibility, not when the workshop paper is camera-ready.

**Stop conditions during the next 4 weeks:**
- If R1's structural surface emerges in spike: stop, pivot to Path B
  in 2 days.
- If two compile errors in the first 50 of ZGC build that *aren't*
  in the docs/05 enumerated categories: stop, do a deeper source
  survey before continuing.
- If MOJO patches break under the pinned OpenJDK 17u SHA: stop,
  align pins.
