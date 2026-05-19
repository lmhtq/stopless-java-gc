# stopless-java-gc

**A CHERI-native concurrent moving garbage collector for the JVM.**

This project is a two-phase research effort to port OpenJDK ZGC to ARM's CHERI
capability hardware (via the Morello platform), and then replace ZGC's software
load barrier with a CHERI-native hardware-checked barrier. The end goal is to
quantify whether hardware capabilities can deliver the same pause-time profile
as ZGC at lower CPU overhead.

The project is being implemented with substantial assistance from Claude Code
+ Claude Opus 4.7 (Anthropic). See `docs/04_risk_register.md` for the AI
collaboration model and what it does/doesn't change about the engineering risk.

## Status

| Phase | Output | Status |
|---|---|---|
| Phase 0 — Repo + design docs + feasibility spike (2 wk) | this repo | **In progress (2026-05)** |
| Phase 1 — ZGC port to CHERI/Morello (3–5 mo) | arXiv preprint #1 (workshop) + repo v0.1 | Pending spike outcome |
| Phase 2 — CHERI-native ZGC barrier (1.5–2.5 mo) | arXiv preprint #2 (full) + repo v1.0 | Blocked on Phase 1 |

## Layout

```
stopless-java-gc/
├── README.md
├── LICENSE                     # Apache 2.0 (patches inherit OpenJDK GPL when applied)
├── CHANGELOG.md
├── .gitignore                  # third_party/, build artifacts, drafts
├── docs/                       # design docs, risk register, ADRs
│   ├── 00_design_overview.md
│   ├── 01_phase_i_zgc_port.md
│   ├── 02_phase_ii_cheri_barrier.md
│   ├── 03_build_setup.md
│   ├── 04_risk_register.md
│   └── _drafts/                # gitignored; meta writeups not yet published
├── scripts/                    # bootstrap + build + benchmark + plot
│   ├── bootstrap.sh
│   ├── apply_patches.sh
│   ├── build_jdk.sh
│   ├── run_tests.sh
│   ├── run_benchmarks.sh
│   └── plot_results.py
├── src/                        # OUR new code (lives outside OpenJDK tree)
│   ├── cap_runtime/            # cap-aware GC bookkeeping, linked into HotSpot
│   └── measurement/            # perf counter harness
├── patches/                    # OUR patches against 3rd-party (committed)
│   └── openjdk-jdk17/          # *.patch files, applied by scripts/apply_patches.sh
├── tests/
│   ├── unit/                   # gtest against src/cap_runtime/
│   ├── integration/            # jtreg + DaCapo on the patched JDK
│   └── bench/                  # perf-counter benchmarks
├── paper/                      # arXiv LaTeX skeletons
│   ├── phase_i.tex
│   ├── phase_ii.tex
│   └── refs.bib
└── third_party/                # gitignored; populated by bootstrap
    ├── cheribuild/
    ├── openjdk-jdk17/
    ├── mojo-patches/
    ├── cheribsd/
    └── cornucopia/
```

## Out-of-tree discipline

**Third-party source is never committed to this repository.** It is cloned into
`third_party/` (gitignored) by `scripts/bootstrap.sh`. Our modifications to
third-party source live in two places:

1. **Net-new code** lives in `src/` and is linked into the OpenJDK build via a
   single hook patch (`patches/openjdk-jdk17/0001-cap-runtime-hook.patch`).
2. **Necessary in-place edits** to existing third-party files are stored as
   `.patch` files under `patches/openjdk-jdk17/`, applied idempotently by
   `scripts/apply_patches.sh`.

This keeps the repo small, the diff against upstream visible, and the
contribution boundary unambiguous for both reviewers and arXiv readers.

## Quickstart

```bash
git clone <this-repo> stopless-java-gc
cd stopless-java-gc
./scripts/bootstrap.sh           # clones third_party, builds Morello SDK
./scripts/apply_patches.sh       # applies patches/openjdk-jdk17/*.patch
./scripts/build_jdk.sh           # builds patched JDK against Morello SDK
./scripts/run_tests.sh           # unit + integration smoke
./scripts/run_benchmarks.sh      # DaCapo + microbench on Morello FVP
```

See `docs/03_build_setup.md` for prerequisites and platform notes.

## Citation

If you reference this work before the preprints land, please cite:

```
Stopless-Java-GC: A CHERI-native concurrent moving GC for the JVM.
Work in progress, 2026. https://github.com/<TBD>/stopless-java-gc
Implemented with assistance from Claude Opus 4.7 via Claude Code (Anthropic).
```

## Prior art

The design and contribution boundary is informed by the existing CHERI×GC
literature. The key works this project sits adjacent to:

- **CHERIvoke** (Xia et al., MICRO 2019) — sweeping CHERI capability revocation
  for C/C++ temporal safety.
- **Cornucopia** (Filardo et al., S&P 2020) — concurrent revocation with a
  small STW phase.
- **Cornucopia Reloaded** (Filardo et al., ASPLOS 2024) — per-page hardware
  capability load barriers on Morello; near-zero pauses.
- **MOJO** (Univ. of Manchester + THG, 2024+) — port of OpenJDK 17 Epsilon /
  Serial / G1 collectors to CheriBSD on Morello. **ZGC is not in MOJO's scope.**
- **ZGC** (Liden, Österlund et al., OpenJDK) — pauseless moving GC with
  colored pointers and software load barriers.

Full references in `paper/refs.bib`.
