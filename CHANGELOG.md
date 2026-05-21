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

### Phase 0 spike — source survey (2026-05-19)
- Real `scripts/run_in_fvp.sh` body: launches FVP_Morello with the
  documented `-C` parameter set (memory, cores, firmware, virtio
  block + net), polls sshd, scp+ssh's the binary into the guest,
  captures exit code, tears down on exit. The placeholder is gone.
- `docs/05_zgc_cheri_collision_report.md` — direct read of
  ZGC's `zAddress.{hpp,inline.hpp}` from OpenJDK 17u tag
  `jdk-17.0.13-ga` on `bc@hasee`. R1 downgraded from Critical to
  High: the conflict is mechanical (every `ZAddress::*` call maps
  to a side-table consult), not structural. ~300 call sites.
- `docs/06_cornucopia_api_survey.md` — direct read of
  `sys/cheri/revoke.h` on CheriBSD `main`. R3 downgraded from
  Medium to Low: `cheri_revoke()` + `cheri_revoke_get_shadow()`
  are arena-based, no kernel patch needed; the JVM presents its
  heap as an arena via `CHERI_REVOKE_SHADOW_NOVMEM`.
- `docs/07_c2_jit_status.md` — literature survey including the
  Feb 2026 *Pitfalls in VM Implementation on CHERI* CRuby paper
  (Liu, Yamazaki, Ugawa). R2 confirmed at High: no public C2-class
  JIT works on CHERI purecap; Phase 1 runs at C1+interpreter
  (MOJO precedent), Phase 2 evaluation reframed against C1 baseline.
- `docs/04_risk_register.md` updated with summary table, evidence
  citations, revised spike day-by-day plan (compressed from 12 to
  ~5 days of empirical confirmation), and a pre-empirical
  **GO** recommendation.

### scripts/bootstrap.sh hardening (2026-05-19)
- `--no-build`, `--check`, `--skip-mojo` flags added.
- Final summary block reports presence/absence of every third-party
  artifact at the end of each run.
- Preflight now distinguishes "tools the script itself needs" from
  "tools cheribuild needs when building the Morello SDK"; the latter
  set fails fast with apt-get / brew install hints rather than dying
  an hour into the SDK build.
- Verified end-to-end on `bc@hasee` against the actual cheribuild
  failure mode (missing `ninja`); the new preflight catches it
  before the SDK build starts.
- `docs/03_build_setup.md` Ubuntu prereq list updated to include
  `ninja-build texinfo nasm`.

### Phase 0 spike — environment fully bootstrapped on bc@hasee (2026-05-20)
- Morello LLVM toolchain built in 3h08m
  (`third_party/output/morello-sdk/bin/clang`).
- CheriBSD buildworld + kernel built in 3h45m, then disk image
  packaged in 61s. Final bootable raw GPT image at
  `third_party/output/cheribsd-morello-purecap.img` (5.57 GiB
  virtual, 2.86 GiB sparse on disk).
- bootstrap.sh extended with `disk-image-morello-purecap` target
  so the .img is produced by default (previously only a rootfs
  directory tree).
- Morello FVP v0.11.34 (Linux64 GCC-6.4) downloaded and
  installed at `third_party/morello-fvp/install/`. Symlinks at
  `third_party/morello-fvp/FVP_Morello` and
  `third_party/output/morello-sdk/FVP_Morello` make both
  `scripts/run_in_fvp.sh` and cheribuild see the binary.
- Morello FVP firmware (TF-A BL31 + SCP/MCP roms + UEFI, ~1 MB)
  fetched via cheribuild's `morello-firmware` target to
  `third_party/output/morello-sdk/firmware/morello-fvp/`.
- First FVP boot of CheriBSD initiated; cheribuild's
  run-fvp-morello-purecap orchestrator wires the .img, firmware,
  and SSH port-forward (host:12345 → guest:22) together.

### Phase 0 spike day 4 — SDK validation + first OpenJDK collision (2026-05-20)
- FVP boot validation: FVP loaded firmware, ran TF-A BL31, EDK2 UEFI,
  and FreeBSD EFI loader to the CheriBSD boot menu. Confirmed FVP
  fully functional. Did NOT proceed past the boot menu — FVP's
  emulated clock is ~30-60× slower than wall time, full boot to
  shell would take 15-30 min wall-clock per attempt, which is not
  the bottleneck on our critical path.
- Cross-compile validation via `tests/integration/hello-cheri/`:
  a 30-line C program compiled with the Morello SDK produced a
  valid CheriBSD purecap ELF. `llvm-readelf -h` confirms
  `Flags: 0x10000, purecap` plus the `.note.cheri` section with
  `NT_CHERI_GLOBALS_ABI=PCREL`, `NT_CHERI_TLS_ABI=MORELLO_MIXED`.
  The SDK is producing fully-conformant Morello binaries.
- MOJO patch series: confirmed not publicly available. Probes of
  GitHub (`mojo-jvm`, `UoM-MOJO`, `CTSRD-CHERI/openjdk-*`,
  `Soteria-Research`) returned 404. The mojo-jvm.org distribution
  URL is a placeholder. Pivoted to **vanilla OpenJDK 17u + our own
  minimal patch series** — better paper novelty anyway.
- `scripts/build_jdk.sh` hardened with: auto-detect Boot JDK 17,
  `--with-toolchain-type=clang`, explicit `--sysroot=` in
  `--with-extra-cflags/ldflags` (OpenJDK autoconf uses `-isysroot`
  for link tests, which Linux clang interprets as headers-only),
  `BUILD_CC=clang BUILD_CXX=clang++` as positional autoconf args.
- **First real R1 collision found**: at `make/autoconf/platform.m4:705`
  OpenJDK refuses cross-compile when `sizeof(int*) != target CPU bits`.
  On Morello purecap, `sizeof(int*) == 16` (128-bit capabilities)
  but `OPENJDK_TARGET_CPU_BITS=64`. Documented in
  `docs/05_zgc_cheri_collision_report.md §5` with a candidate patch.
  This is the first entry in the real patch series (will be
  `patches/openjdk-jdk17/0002-platform-accept-cheri-cap-ptr-size.patch`).
- Apt installs on `bc@hasee` (sudo, password supplied):
  ninja-build, libtool-bin, libarchive-dev, openjdk-17-jdk-headless,
  clang (host).
- Disk: `/` was 100% full at one point (1.7T used by other projects).
  User cleaned up to 101 GB free; project's build artifacts now
  go to `/mnt/nas/lxhL3/projs/stopless-build/` via symlink at
  `~/projs/stopless-java-gc/build/`.

### Pending — Phase 1 patch series (the real work)
The Phase 0 spike has concluded. R1 is empirically confirmed
structural-but-mechanical; R2 and R3 stayed at their source-survey
verdicts (R2 High, R3 Low). Phase 1 begins with:
- `0001-cap-runtime-hook.patch` (build-system hook for src/cap_runtime/)
- `0002-platform-accept-cheri-cap-ptr-size.patch` (the new finding)
- Subsequent patches surface as `configure` and then `make` proceed
  through each layer of CHERI-cap incompatibility.

### Phase 1 day 1 (2026-05-21): 6 patches land, build into HotSpot
- 0002 platform.m4 — accept 128-bit ptr on aarch64
- 0003 JvmMapfile.gmk — share linux's nm branch with bsd
- 0004 flags-cflags.m4 — set _ALLBSD_SOURCE -D_GNU_SOURCE on JVM
- 0005 flags-cflags.m4 — extend to also set -DBSD -D_FILE_OFFSET_BITS=64
  on JVM (without -DBSD, semaphore.hpp falls through to #error)
- 0006 bytes_bsd_aarch64.hpp — add __FreeBSD__ branch using
  <sys/endian.h> + __bswap{16,32,64}
- 0007 bitMap.hpp — bm_word_t = uint64_t (was uintptr_t which on
  CHERI = __intcap = 128 bits, breaking the static assert + the
  count_trailing_zeros overload)

All 6 idempotent through `scripts/apply_patches.sh` against a clean
OpenJDK 17u (jdk-17.0.13-ga) checkout. `configure` completes
successfully; `make images` reaches HotSpot compilation and surfaces
the next layer of collisions catalogued at docs/05 §6 patches 0008–0011.

### Phase 1 day 1 — next-layer findings (pending patches)
- 0008 shenandoahMarkBitMap — Shenandoah's own bm_word_t needs the
  same uintptr_t → uint64_t change (Shenandoah declares it
  independently of share/utilities/bitMap.hpp)
- 0009 semaphore.hpp / build flags — `-DBSD` from patch 0005 is
  leaking into buildjdk (the HOST x86 linux JDK built during
  bootstrap), making it try to include semaphore_bsd.hpp. Patch
  needs to scope the bsd-only JVM defines to TARGET, not bleed to
  BUILD. Investigation pending.
- 0010 macroAssembler_aarch64.hpp:523 — `mov(reg, intptr_t)`
  ambiguous because under CHERI `intptr_t = __intcap` matches
  multiple existing `mov` overloads. Either add a CHERI-aware
  overload or rename one.
