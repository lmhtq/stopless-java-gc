# Handoff: Mac-based Claude session → hasee-based Claude session

**Date:** 2026-05-27, end of session
**Last commit:** `d8bf570` on gitee.com:lmhtq/stopless-java-gc.git, branch main
**Reason for switch:** authoring patches on Mac without access to
`third_party/openjdk-jdk17/` source led to wrong line numbers, wrong
file-structure assumptions, and a long debugging cycle. hasee has the
source — work there to fix this loop.

## Read this first if you are a new Claude session on hasee

You have NO conversation history from the prior Mac session. Below is
the minimal state you need:

### The project

**Stopless-Java-GC** — a CHERI-native moving GC for OpenJDK 17u on
ARM Morello. Paper target: workshop paper, ~3 months. Pivoted from
"ZGC barrier swap" to "fresh CHERI-native moving GC subsystem" early
on; the latter is novel because Cornucopia Reloaded already covered
the cap-load-barrier angle.

Key conceptual references:
- `docs/15_cheri_stopless_design.md` — paper §3 draft
- `docs/17_phase_c_overview.md` — 12-week roadmap, you are mid-W2

### What works (verified on hasee + CheriBSD QEMU purecap C64)

Stage A of `scripts/c_phase_verify.sh`:
- `test_basic` — single-object move + revoke + handler fault: exit 0
- `test_multi` — 32 objects × 2 threads, batched revoke: exit 0
- `test_alloc` — bump allocator, varying-size + OOM + reset: exit 0
- `test_alloc_concurrent` — 4 threads × 20k allocs no-overlap: exit 0

These are the headline results for paper §4 (mechanism). The
`cap_runtime/stopless_gc/` C library is solid.

### What partially works

JVM-side integration (Phase C-1 + C-2):
- `apply_patches.sh` applies 0080+0081 cleanly to a pristine tree
- libjvm.so builds with StoplessGC compiled in
- `-XX:+UseStoplessGC` recognized, identified in version banner as
  "stopless gc, bsd-aarch64"
- BUT: `java -XX:+UseStoplessGC -version` crashes with
  "In-address space security exception" (SIGPROT) during early init

### Immediate next steps (ordered by criticality)

1. **C-3 + C-4 actually applied**: patches 0083 (StoplessArena C++)
   and 0085 (allocate wiring) are still hand-authored and won't apply
   cleanly. Use the SAME workflow that fixed 0080/0081:

   ```bash
   # On hasee:
   cd ~/projs/stopless-java-gc/third_party/openjdk-jdk17
   # Apply the INTENT manually by editing files directly
   # ... (write a python script like scripts/apply_stoplessgc_to_openjdk.py
   #     for C-3/C-4 intent)
   # Then:
   git add src/hotspot/share/gc/stopless/stoplessArena.{hpp,cpp}
   git diff --cached -- src/hotspot/share/gc/stopless/stoplessArena.hpp \
                       src/hotspot/share/gc/stopless/stoplessArena.cpp \
       > ~/projs/stopless-java-gc/patches/openjdk-jdk17/0083-regen.patch
   git diff -- src/hotspot/share/gc/stopless/stoplessHeap.{hpp,cpp} \
       > ~/projs/stopless-java-gc/patches/openjdk-jdk17/0085-regen.patch
   # Manually combine with header + replace existing patches
   ```

2. **C-6 (shift=64)**: blocking `java -version`. This is the Phase 1
   residual from `docs/14_handoff_shift64_and_pivot.md`. Multiple
   wordSize misuse sites cause sbfm/ubfm encoder shift to overflow.
   Strategy: instrument + bisect like patches 0068/0069/0070 did.

3. **C-5 acceptance**: `java -XX:+UseStoplessGC -version` exits 0.
   Possibly blocked by C-6.

### Repo state at handoff commit `d8bf570`

```
docs/
├── 17_phase_c_overview.md            # roadmap
├── 18_handoff_to_hasee_session.md    # THIS FILE
├── c1/ design.md impl_notes.md test.md
├── c2/ design.md impl_notes.md test.md
├── c3/ design.md impl_notes.md test.md
└── c4/ design.md impl_notes.md test.md

patches/openjdk-jdk17/
├── 0080-stopless-gc-skeleton.patch          # CANONICAL (regen from hasee, applies clean)
├── 0081-stopless-gc-feature-enable.patch    # CANONICAL (regen from hasee, applies clean)
├── 0082-stopless-runtime-link.patch         # STUB (folded into 0081)
├── 0083-stopless-arena-cpp-bridge.patch     # HAND-AUTHORED, needs regen on hasee
└── 0085-stopless-arena-allocate-wire.patch  # HAND-AUTHORED, needs regen on hasee

src/cap_runtime/stopless_gc/                 # works on QEMU, Stage A all pass
├── allocator.{c,h}                          # bump-pointer + CHERI csetbounds
├── revoke.{c,h}                             # cheri_revoke + shadow bitmap
├── forward_table.{c,h}                      # concurrent hashmap
├── handler.{c,h}                            # SIGPROT handler
├── api.h
└── tests/                                   # 4 binaries

scripts/
├── apply_patches.sh                         # main applier (idempotent)
├── apply_stoplessgc_to_openjdk.py           # ★ workflow template — used to fix 0081
├── fix_patch_counts.py                      # ★ recompute @@ counts in hand-authored patches
├── c_phase_verify.sh                        # 4-stage A→D test harness
└── fast_iter.sh                             # 80s build+ship cycle
```

### Critical state to know on hasee

- Working dir: `~/projs/stopless-java-gc/`
- OpenJDK source: `~/projs/stopless-java-gc/third_party/openjdk-jdk17/`
- OpenJDK base SHA: `eed263c8077e066a32d746fd188f2dee8c47b9ab`
- Build dir: `~/projs/stopless-java-gc/third_party/openjdk-jdk17/build/jdk-morello/`
- libjvm.so (post-d8bf570): contains StoplessGC subsystem, but
  `mem_allocate` returns nullptr because 0083+0085 not yet applied
- Patch-state file: `third_party/openjdk-jdk17/.stopless/patch-state`
  marks 0002-0070 + 0080 + 0081 as applied. Edit this carefully if
  you want apply_patches.sh to reapply 0083+0085.
- QEMU guest: ssh -p 10005 root@localhost (from hasee). /opt/jdk/
  pre-extracted. Use fast_iter.sh to ship libjvm.so to it.

### Things you SHOULD NOT do

- Don't hand-author patches without Reading the actual openjdk files
  first. That's how we wasted hours on the Mac.
- Don't `git reset --hard` the openjdk tree without a snapshot —
  it's not committed to any remote.
- Don't delete `third_party/openjdk-jdk17/.stopless/patch-state`
  without a backup — losing it forces re-application of all 60+
  patches, some of which may now collide.
- Don't run `bash configure` lightly — it overwrites
  `build/jdk-morello/spec.gmk` which we've hand-edited to include
  `stoplessgc` in VALID_JVM_FEATURES.

### User context (preserve from Mac-session memory files)

- User is a broad-vision generalist using LLMs to implement
  cross-domain ideas
- This CHERI×Java GC project is his current spark
- Target: T3-A = full JVM integration + microbench + workshop paper
- He prefers being explicit about risk and tradeoffs
- He doesn't want destructive actions taken without confirmation
- He uses gitee not github; lmhtq1991@gmail.com, name lmhtq
- bc@hasee for all builds, sudo password "asdfgh" pre-authorized
- third_party/ stays gitignored
- No hardware access (no Morello board); CHERI-QEMU is the test env

### Verification helper

`scripts/c_phase_verify.sh` runs 4 stages:
- A: cap_runtime tests on QEMU
- B: patch dry-run via `git apply --check`
- C: hotspot build via `gmake hotspot-server-libs`
- D: `java -XX:+UseStoplessGC -version` in QEMU

Currently A passes, B/C/D have the issues documented above.
