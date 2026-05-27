# Progress snapshot — 2026-05-27 end of session

A point-in-time summary of where everything is. Use this if you
just need the 5-minute overview without reading all the design
docs.

## Headline

`-XX:+UseStoplessGC` works at JVM level (recognized + selected,
shows "stopless gc" in version banner). Crashes during VM init for
known reasons (mem_allocate not yet wired + Phase 1 shift=64
residual). Mechanism layer (cap_runtime, paper §4) is solid: all 4
tests pass on QEMU.

## Phase status

| Phase | Status | Note |
|---|---|---|
| Phase 1 (CHERI build bring-up) | 95% | shift=64 sbfm/ubfm residual blocks java-version |
| Phase 1.5 (16-byte HeapWord) | ✅ done | patches 0060/0064/0065/0066/0068 |
| Phase A (cap_runtime primitives) | ✅ done | revoke + handler + forward_table |
| Phase B (multi-thread mechanism) | ✅ done | test_multi passes |
| Phase C-1 (HotSpot skeleton) | ✅ done | patch 0080 applies clean |
| Phase C-2 (flag/build wire-up) | ✅ done | patch 0081 applies clean |
| Phase C-3 (StoplessArena C++) | 50% | hand-authored patch, needs regen |
| Phase C-4 (bump allocator) | ✅ + 50% | C side done + tested; JVM wire-up patch needs regen |
| Phase C-5 (java -version) | ❌ blocked | needs C-3+C-4 patches applied + C-6 |
| Phase C-6 (shift=64) | ❌ pending | Phase 1 residual, back on critical path |
| Phase C-7..C-12 | ⬜ not started | |

## Test results

`scripts/c_phase_verify.sh` last run:

| Stage | Outcome | Time | Note |
|---|---|---|---|
| A — cap_runtime tests | ✅ PASS (4/4) | 31s | test_basic, test_multi, test_alloc, test_alloc_concurrent |
| B — patch dry-run | 🟡 partial | 2s | 0080/0081 clean; 0083/0085 still hand-authored |
| C — hotspot build | ✅ PASS | varies | libjvm.so 133M with stoplessgc |
| D — java -version | ❌ FAIL | 64s | SIGPROT during VM init |

## Numbers worth quoting in paper

- cap_runtime end-to-end concurrent: **collector 32 moves, mutator
  259,936 reads, 1 handler fault, exit 0** in 3s on QEMU C64 purecap
- Concurrent allocator: **80k allocs / 4 threads, no overlap** verified
- Classes loaded after patch 0068: **13 → 146** (single 26-line patch)
- StoplessGC LoC budget: target ~3k vs ZGC's ~28k (~10× smaller)

## Files inventory

### Documentation
```
docs/00_design_overview.md
docs/01_phase_i_zgc_port.md           (early — superseded)
docs/02_phase_ii_cheri_barrier.md     (early — superseded)
docs/03_build_setup.md
docs/04_risk_register.md
docs/05_zgc_cheri_collision_report.md
docs/06_cornucopia_api_survey.md
docs/07_c2_jit_status.md
docs/08_phase_1_results.md
docs/09_class_injected_fields_cap_provenance.md
docs/10_cheri_seam_scan.md
docs/11_stack_trace_class_mirror_allocate.md
docs/12_simd_codegen_wordsize_seam.md
docs/13_session_summary_2026_05_27.md
docs/14_handoff_shift64_and_pivot.md
docs/15_cheri_stopless_design.md       # paper §3 draft
docs/16_caprev_status.md
docs/17_phase_c_overview.md            # ★ roadmap
docs/18_handoff_to_hasee_session.md    # ★ for next Claude session
docs/19_decisions_log.md               # ★ why we chose what we chose
docs/20_c3_c4_regen_playbook.md        # ★ exact next steps
docs/21_progress_snapshot.md           # ★ THIS FILE
docs/c1/ design.md impl_notes.md test.md
docs/c2/ design.md impl_notes.md test.md
docs/c3/ design.md impl_notes.md test.md
docs/c4/ design.md impl_notes.md test.md
```

### Patches
```
patches/openjdk-jdk17/
  0002..0070  (Phase 1 — CHERI build bring-up, all applied)
  0080         Phase C-1 HotSpot skeleton (CANONICAL)
  0081         Phase C-2 flag wire-up (CANONICAL)
  0082         (stub, content folded into 0081)
  0083         Phase C-3 StoplessArena (HAND-AUTHORED, needs regen)
  0085         Phase C-4 allocate wiring (HAND-AUTHORED, needs regen)
```

### cap_runtime (works on QEMU)
```
src/cap_runtime/stopless_gc/
  api.h
  revoke.h revoke.c              # arena + cheri_revoke + shadow
  allocator.h allocator.c         # bump-pointer + CHERI csetbounds
  forward_table.h forward_table.c # concurrent open-addressed hash
  handler.h handler.c             # SIGPROT handler
  Makefile
  tests/
    test_basic.c       # 1 obj move + 1 fault — PASS
    test_multi.c       # 32 obj × 2 threads — PASS
    test_alloc.c       # varying sizes + OOM + reset — PASS
    test_alloc_concurrent.c  # 4 threads × 20k — PASS
```

### Scripts
```
scripts/
  apply_patches.sh                       # idempotent applier
  bootstrap.sh
  build_jdk.sh
  fast_iter.sh                           # 80s build+ship cycle
  run_in_fvp.sh
  run_in_qemu.sh
  run_tests.sh
  scan_cheri_seams.py                    # CHERI source scanner (Phase 1 utility)
  plot_results.py
  upstream_pins.env
  c_phase_verify.sh                      # ★ Phase C 4-stage tester
  fix_patch_counts.py                    # ★ patch @@ count fixer
  apply_stoplessgc_to_openjdk.py         # ★ template for OpenJDK edits on hasee
  apply_0081_intent_on_hasee.sh          # forensic (failed-regex earlier attempt)
```

## State on hasee (verified at session end)

- `~/projs/stopless-java-gc/` checked out
- `third_party/openjdk-jdk17/` at base SHA `eed263c8077e066a32d746fd188f2dee8c47b9ab`
- `.stopless/patch-state` ends with `APPLIED=0081-stopless-gc-feature-enable.patch`
- `build/jdk-morello/spec.gmk` hand-patched to include `stoplessgc` in
  VALID_JVM_FEATURES + JVM_FEATURES_server
- `libjvm.so` built with stoplessgc symbols, timestamp 21:17
- libstopless_gc.a built and linked
- `~/.nvm/versions/node/v20.11.1/bin/claude` v2.1.152 installed but
  not in default PATH

## Gitee state

- Remote: `git@gitee.com:lmhtq/stopless-java-gc.git`
- main HEAD: `f7cf91e` (docs/18 handoff)

## Risks tracked

1. **shift=64 sbfm/ubfm** (Phase 1 residual) — multiple wordSize
   misuse sites cause aarch64 shift encoder to overflow during early
   VM init. C-6 work.
2. **C-3+C-4 patches are hand-authored** — won't apply to a pristine
   tree until regenerated using `docs/20_c3_c4_regen_playbook.md`.
3. **spec.gmk in-place patching** — recomputing the build dir via
   `bash configure` will undo this. Acceptable for now; revisit if
   build-dir rebuilds become routine.
4. **No write barrier** — concurrent move correctness requires C-10.
5. **No root scanner** — moving GC needs C-8 to find live references.
6. **C2 JIT disabled** — clang ICE on Morello. We live with C1 +
   interpreter. Acceptable for paper.
7. **JNI native callers** — must pin or accept JNI breakage when GC
   moves; v1 pins all JNI refs.

## What "done" means for the paper

A passing acceptance demo for the workshop paper would be:

```bash
$ java -XX:+UseStoplessGC -Xms64m -Xmx64m -cp . AllocChurn
# Outputs:
#   Allocated N million objects
#   GC moves: M, faults: K, pause-time-max: 0 ms
#   ✓ no STW pauses observed
```

We are NOT there yet. Path to it: C-3..C-12 (~10 weeks).

## Critical contact between sessions

Read `docs/18_handoff_to_hasee_session.md` first. Then read this
file. Then `docs/19_decisions_log.md` if making any architectural
choices. Then `docs/20_c3_c4_regen_playbook.md` for next concrete
work.
