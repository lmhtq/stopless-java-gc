# Decisions log — why we are where we are

A running log of architectural decisions and the reasons behind them.
Future-you reads this when you wonder "why did we do X instead of Y?"

## D1: Stopless GC + CHERI, not ZGC + CHERI barrier swap

**Date:** early in Phase 2
**Decision:** Don't replace ZGC's load barrier with a CHERI cap-tag
check (initial idea). Instead, build a fresh moving GC inspired by
Stopless (Pizlo 2007), where the CHERI cap-tag IS the barrier.

**Why:**
- Cornucopia Reloaded (MSR 2024) already covered the "cap-load barrier
  + revocation sweep" angle. Doing it again on ZGC would be a delta-
  paper at best.
- Stopless's lock-free moving invariants (MI/CI/NS) have never been
  combined with CHERI hardware before — clear novelty.
- ZGC is ~28k LoC; a fresh GC reusing CHERI hw could be ~3k LoC. The
  "small GC" hook is easier to pitch.

**Refs:** docs/15_cheri_stopless_design.md §1-3.

## D2: 16-byte HeapWord on CHERI purecap

**Date:** Phase 1.5
**Decision:** Redefine `HeapWordSize = sizeof(HeapWord) = 16` on
purecap CHERI (vs traditional 8 on LP64).

**Why:**
- Cap-sized oop fields are 16 bytes on Morello.
- Patch 0060 introduced `struct HeapWord { ... 16 bytes ... }`.
- Without this, oop arrays + alignments break in cascading ways.
- Aligns with MOJO's published approach (same project family).

**Side effect:** ObjectAlignmentInBytes default of 8 makes
MinObjAlignment = 8/16 = 0 → align_object_size collapses to 0 → every
instance has size 0 → memset overflow. Fixed by patch 0068 which
auto-bumps ObjectAlignmentInBytes ≥ HeapWordSize under purecap.

**Refs:** patches 0060, 0064, 0065, 0068; docs/11.

## D3: T_ADDRESS BasicType for cap-typed injected fields

**Date:** Phase 2
**Decision:** Mark CHERI-cap-bearing klass / vmtarget / etc fields
with BasicType `T_ADDRESS` and signature char `"T"` (custom).

**Why:**
- These fields hold true CHERI caps with provenance; treating them as
  `intptr_t` strips the tag at store time.
- MOJO uses exactly this convention. Published in their CHERI Tech 23
  slides. We aligned to avoid reinventing.

**Refs:** patch 0066, docs/09.

## D4: ObjectAlignmentInBytes auto-bump (Phase 1 single-largest win)

**Date:** Phase 1
**Decision:** When CHERI purecap, force ObjectAlignmentInBytes ≥
HeapWordSize at flag-parse time.

**Why:** See D2 "side effect". Classes-loaded jumped from 13 → 146
after this 26-line patch. Single biggest unblocking change in Phase 1.

**Refs:** patch 0068.

## D5: Fresh GC subsystem, not patching ZGC/G1/Serial/Epsilon

**Date:** start of Phase C (today)
**Decision:** New `src/hotspot/share/gc/stopless/` directory rather
than modifying any existing GC.

**Why:**
- ZGC/G1 are 28k LoC each — editing them risks breaking known-good
  configurations and adds debug surface.
- CHERI hw replaces 4 ZGC subsystems for free (load barrier, mark
  bitmap, card table, generations) — those would be dead code if we
  patched ZGC.
- A clean directory makes the paper title easier: "A 3k-line moving
  GC enabled by CHERI capabilities."
- Epsilon is the smallest real GC (~700 LoC) — useful as a structural
  template.

**Refs:** docs/17 §2, docs/c1/design.md.

## D6: No TLAB in v1

**Date:** Phase C-4
**Decision:** `UseTLAB = false` in StoplessHeap::initialize(). All
allocations go through `mem_allocate` (one CAS into the bump
allocator).

**Why:**
- TLAB adds complexity (per-thread refill, slow-path fallback,
  inline-cache JIT integration).
- Single global CAS is fast enough for v1; profile under DaCapo
  before adding TLAB.
- Allows us to focus C-4 effort on the allocator itself.

**Risk:** allocator contention under multi-thread mutator workloads.
Acceptable for paper §5 because we control benchmark.

## D7: Hand-author patches blind from Mac (anti-pattern — DON'T repeat)

**Date:** today's session
**Decision (now reversed):** Author Phase C patches on Mac without
reading actual OpenJDK files; rely on Mac as primary workstation.

**Why it failed:**
- Wrong @@ hunk line counts (had to write `fix_patch_counts.py`).
- Wrong assumptions about OpenJDK 17u internal structure
  (BarrierSet is x-macro, GC selection is in gcConfig.cpp not
  arguments.cpp, missing pure-virtuals in CollectedHeap).
- Each wrong assumption → SSH grep + sed → debug round.
- Total wasted effort: ~half a session.

**Reversal:** Work directly on hasee where OpenJDK source is
Readable. Use the "apply intent + git diff" workflow (see
`scripts/apply_stoplessgc_to_openjdk.py` as template).

**Refs:** docs/18 handoff, this session's commit chain.

## D8: Process for adding to existing OpenJDK files

**Pattern (established today, validated):**
1. SSH to hasee.
2. `cd third_party/openjdk-jdk17 && cat <target_file>` — read first.
3. Write a Python script that uses exact-string substitution (not
   regex) to apply the intent.
4. Run script; verify with `git diff`.
5. `git diff --cached` (after `git add` for new files) captures the
   canonical patch.
6. Copy patch back to local repo's `patches/openjdk-jdk17/`.
7. Test apply with `git apply --check` from pristine state.

**Why not regex:** OpenJDK files use idiosyncratic indentation,
line continuations (`\` for macros), trailing whitespace. Regex
patterns rarely match across them.

**Refs:** `scripts/apply_stoplessgc_to_openjdk.py`.

## D9: Patch numbering convention

**Decision:**
- 0001-0070: Phase 1 (CHERI bring-up of OpenJDK build)
- 0080-0089: Phase C (StoplessGC integration)
- (0071-0079 reserved as gap for late Phase 1 follow-ups)

Within Phase C:
- 0080: skeleton (new files in gc/stopless/)
- 0081: feature wire-up (modifications across 8 shared files)
- 0082: SUPERSEDED stub (content folded into 0081 to avoid line drift)
- 0083: StoplessArena C++ class
- 0085: Allocator wire-up (uses cap_runtime)
- 0084 reserved (cap_runtime additions live outside openjdk tree)

## D10: cap_runtime as separate static lib, linked into libjvm.so

**Decision:** `src/cap_runtime/stopless_gc/libstopless_gc.a` is built
independently (with morello-sdk clang), then linked into libjvm.so via
JvmFeatures.gmk.

**Why:**
- Keeps the C runtime testable standalone (test_basic, test_multi,
  test_alloc, test_alloc_concurrent — all pass on QEMU).
- Decouples cap_runtime fixes from hotspot build cycle.
- Paper §4 mechanism evaluation runs entirely in C land — JVM
  integration is paper §5, not §4.

**Refs:** JvmFeatures.gmk stoplessgc block (in patch 0081), cap_runtime/Makefile.

## D11: _Atomic size_t in C struct visible to C++

**Decision:** `stopless_arena_t::bump_offset` declared as
`_Atomic size_t` in C, but typedef-mapped to plain `size_t` when
included from C++.

**Why:** `_Atomic` is C11, not parseable by C++. C++ never reads the
field directly (all access via C functions); so the ABI-compatible
type-erasure is safe on aarch64 (same size + alignment).

**Refs:** `src/cap_runtime/stopless_gc/revoke.h` STOPLESS_ATOMIC_SIZE_T.

## D12: spec.gmk in-place patch (not configure re-run)

**Decision:** When 0081 adds `stoplessgc` to jvm-features.m4, the
generated `build/jdk-morello/spec.gmk` doesn't pick it up
automatically. We patch spec.gmk in place with sed rather than
re-running `bash configure`.

**Why:**
- `configure` is slow (~3 min on hasee) and regenerates many files,
  potentially undoing other in-tree state.
- spec.gmk only has 2 lines that need editing
  (`VALID_JVM_FEATURES`, `JVM_FEATURES_server`).
- The proper fix is to commit to re-configuring whenever
  jvm-features.m4 changes, but that's overhead we don't yet need.

**Risk:** if hasee's build dir is recreated (e.g., `make clean`), the
spec.gmk edit is lost. Mitigation: capture the sed in a script if it
becomes a recurring issue.
