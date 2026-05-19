# Changelog

All notable changes to this project will be documented in this file.

The format is loosely based on [Keep a Changelog](https://keepachangelog.com/),
and the versions correspond to project phase milestones (see README).

## [Unreleased] — Phase 0 (repo init)

### Added
- Apache-2.0 LICENSE and README outlining two-phase plan.
- `.gitignore` excluding `third_party/`, build artifacts, drafts.
- `CHANGELOG.md`.
- `docs/_drafts/origin-writeup.md` — meta writeup that triggered the project
  (not yet published; will be revised after Phase 1 produces evidence).
- Directory scaffold: `docs/`, `scripts/`, `src/{cap_runtime,measurement}/`,
  `patches/openjdk-jdk17/`, `tests/{unit,integration,bench}/`, `paper/`,
  `third_party/`.
- `third_party/README.md` documenting which upstreams are cloned and where.
- `patches/README.md` documenting out-of-tree patch discipline.
- 5 substantive design documents under `docs/`:
  - `00_design_overview.md` — problem, four-claim decomposition, two-phase plan.
  - `01_phase_i_zgc_port.md` — ZGC's three CHERI-hostile features, design
    space, tentative choices, ~10–17 kLOC budget, workshop paper outline.
  - `02_phase_ii_cheri_barrier.md` — cap-load barrier swap, SIGCAPRVOKE
    handler, Cornucopia driver, falsifiable claim.
  - `03_build_setup.md` — prereqs, bootstrap, build, run on QEMU/FVP.
  - `04_risk_register.md` — top 6 risks, mitigations, 2-week feasibility
    spike, go/no-go criteria; explicit on what AI assistance does and
    doesn't change.
- Build infrastructure under `scripts/`:
  - `bootstrap.sh` — idempotent upstream clone + Morello SDK build.
  - `upstream_pins.env` — pinned SHAs / URLs / SHA256s.
  - `apply_patches.sh` — idempotent patch application driven by
    `patches/openjdk-jdk17/series`.
  - `build_jdk.sh` — configure + make for Morello (default) or x86.
  - `run_in_qemu.sh` / `run_in_fvp.sh` — invocation harnesses (bodies
    pending Phase 0 spike S1).
  - `run_tests.sh` — drives gtest + integration smoke.
  - `run_benchmarks.sh` — DaCapo runs with JFR + PMU capture.
  - `plot_results.py` — matplotlib plots for throughput / pauses / barrier
    cycles.
- Out-of-tree C++ in `src/cap_runtime/`:
  - C-compatible public ABI (`cap_runtime.h`) with version gate.
  - `side_table.{h,cc}` — shadow bitmap color storage.
  - `forwarding_table.{h,cc}` — lock-free open-addressed forwarding hash.
  - `revoke_glue.{h,cc}` — Cornucopia Reloaded integration (Phase 2 stubs).
- Measurement helper `src/measurement/pmu_sample.{h,cc}` (Phase 0 stub).
- Top-level `CMakeLists.txt` detecting `__CHERI_PURE_CAPABILITY__`, exporting
  `compile_commands.json`, providing `-DCAP_RUNTIME_BUILD_TESTS=ON` and
  `-DCAP_RUNTIME_BUILD_BENCH=ON`.
- `.clangd` config so editors treat .cc/.h as C++17 before cmake runs.
- 12 gtest unit tests under `tests/unit/`:
  - `test_cap_runtime.cc` — heap lifecycle, ABI version.
  - `test_side_table.cc` — color load/store/flip.
  - `test_forwarding_table.cc` — install/lookup/multi-thread race (8 thr × 5k iters).
- `tests/integration/run_dacapo_smoke.sh` — Phase 1 pass/fail gate.
- `tests/bench/microbench_pointer_chase.cc` — raw vs. simulated-ZGC-barrier
  pointer-chase.
- arXiv paper skeletons under `paper/`:
  - `phase_i.tex` — workshop paper, ~70% drafted (intro / background /
    design / impl prose).
  - `phase_ii.tex` — main paper, section scaffolding only.
  - `refs.bib` — bibliography (CHERIvoke, Cornucopia, Cornucopia Reloaded,
    MOJO, ZGC, Shenandoah, G1, C4, hazard pointers, RCU, Morello, CHERI ISA).
  - `paper/README.md` — build instructions + arXiv submission checklist.
- Patch series at `patches/openjdk-jdk17/series` listing planned patches
  for Phase 1 and Phase 2.
- `0001-cap-runtime-hook.patch.template` documenting the intended hook
  patch contract before a real OpenJDK checkout exists.

### Verified
- `cmake --build build` produces `libstopless_cap_runtime.a` and
  `libstopless_measurement.a` cleanly on macOS aarch64 (AppleClang 16).
- `ctest --test-dir build` → 12/12 tests pass (0.03 s).

### Pending
- Phase 0 feasibility spike (2 weeks): retire risks R1 (ZGC×CHERI
  fundamental incompatibility), R2 (C2 cap-awareness), R3 (Cornucopia
  revoke API surface) before committing to the 5–7 month Phase 1+2 plan.
