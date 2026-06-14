# Phase C overview — StoplessGC in HotSpot (T3-A)

**Status:** roadmap, locked target
**Date:** 2026-05-27
**Decision:** push to T3-A = full JVM integration, fresh GC subsystem
designed from scratch (not modifying ZGC/G1/Serial/Epsilon)
**Owner:** bluecat

This document is the **single source of truth** for Phase C. Every
sub-phase (C-1 through C-12) has its own design + impl + test
artifacts referenced from here.

## 1. Goal

A Java program runs **without source or bytecode changes**:

```
$ java -XX:+UseStoplessGC -Xmx2g MyApp
```

Expected behaviour:
* GC pause time ≈ **0 ms** (only short root-scan STW remains in v1)
* Mutator fast-path read barrier = **0 software instructions**
  (hardware cap-tag check is the barrier)
* Throughput within ε of Epsilon (no-op) GC for non-GC-pressure
  benchmarks
* No requirement to rebuild user code

## 2. Why a fresh GC, not patches on ZGC

**Code-size leverage.** ZGC/G1 are ~28k LoC each (load barriers,
forwarding tables, mark bitmaps, card tables, generations, etc).
CHERI hardware replaces 4 of those subsystems for free:

| Subsystem | ZGC LoC | StoplessGC LoC | Why we can drop |
|---|---|---|---|
| Load barrier asm/helpers | ~6,000 | **0** | hw cap-tag check |
| Mark bitmap | ~3,000 | **0** | trace by walking refs |
| Card table / remset | ~2,000 | **0** | cap-tag is per-pointer |
| Generations | ~5,000 | **0** | single arena, continuous |
| Forwarding tables | ~2,500 | ~300 | side hash map |
| Safepoint glue | ~4,000 | ~200 | only for root scan |
| Allocator | ~3,000 | ~500 | bump pointer + csetbounds |
| CollectedHeap impl | ~2,500 | ~800 | minimum required virtuals |
| **Total** | **~28,000** | **~3,000** | **-89%** |

Paper title gain: *"A 3000-line moving GC enabled by CHERI
capabilities."* That hook is easier to pitch than *"We patched ZGC
to remove the colored-pointer mask check."*

## 3. Architecture

```
╔════════════════════════════════════════════════════════════════════╗
║   JAVA APPLICATION (no changes, no recompile of bytecode)          ║
║                                                                     ║
║   $ java -XX:+UseStoplessGC -Xmx2g MyApp                            ║
╚════════════════════════════════════════════════════════════════════╝
                              │
                              ▼
╔════════════════════════════════════════════════════════════════════╗
║   HOTSPOT JVM (purecap C64 build, our patches)                      ║
║                                                                     ║
║   ┌──────────────────────────────────────────────────────────┐     ║
║   │ Interpreter / C1 JIT (UNCHANGED, purecap-native)          │     ║
║   │   getfield  →  ldr c0, [c1, #offset]   ← hw cap-tag check│     ║
║   │   putfield  →  str c0, [c1, #offset]                     │     ║
║   │   aaload    →  ldr c0, [c1, c2, lsl#4]                   │     ║
║   │   ⇒ Read barrier = 0 software instructions                │     ║
║   └──────────────────────────────────────────────────────────┘     ║
║                              │                                      ║
║                              ▼                                      ║
║   ┌──────────────────────────────────────────────────────────┐     ║
║   │ src/hotspot/share/gc/stopless/ ← NEW (Phase C)            │     ║
║   │                                                            │     ║
║   │  StoplessHeap : CollectedHeap                              │     ║
║   │  StoplessArguments : GCArguments     (-XX:+UseStoplessGC) │     ║
║   │  StoplessBarrierSet : BarrierSet     (read=NOP)           │     ║
║   │  StoplessArena                       (wraps cap_runtime)  │     ║
║   │  StoplessAllocator                   (bump + csetbounds)  │     ║
║   │  StoplessRootScanner                 (JavaThread + JNI)   │     ║
║   │  StoplessCollectorThread : ConcurrentGCThread             │     ║
║   │  stopless_jvm_bridge                 (SIGPROT install)    │     ║
║   └──────────────────────────────────┬───────────────────────┘     ║
║                                      │                              ║
║                                      ▼                              ║
║   ┌──────────────────────────────────────────────────────────┐     ║
║   │ cap_runtime/stopless_gc/  ← ✅ DONE (Phase A/B)            │     ║
║   │   revoke.c  forward_table.c  handler.c  api.h             │     ║
║   └──────────────────────────────────────────────────────────┘     ║
╚════════════════════════════════════════════════════════════════════╝
```

## 4. 12-week roadmap

| Week | Phase | Deliverable |
|---|---|---|
| W1-W2 | **C-1** | StoplessGC HotSpot skeleton (compiles, prints "hello") |
| W2 | **C-2** | `-XX:+UseStoplessGC` flag plumbing |
| W3 | **C-3** | StoplessArena C++ wrapper (talks to cap_runtime) |
| W3-W4 | **C-4** | StoplessAllocator (bump + csetbounds + SW_VMEM strip) |
| W4 | **C-5** | `java -XX:+UseStoplessGC -version` 跑通 (no moves) |
| W5 | **C-6** | shift=64 sbfm/ubfm bug 收尾 (重新加回 critical path) |
| W6 | **C-7** | SIGPROT handler 在 JVM 进程内安装 |
| W6 | **C-8** | Root scanner (JavaThread stack + JNI + system dictionary) |
| W7-W8 | **C-9** | StoplessCollectorThread (concurrent mover) |
| W9 | **C-10** | Write barrier (concurrent move correctness) |
| W10 | **C-11** | Microbench (GC freq × obj size × alloc rate sweep) |
| W11 | **C-12a** | DaCapo subset: lusearch, fop, h2 |
| W12 | **C-12b** | Paper §5 / §6 draft |

## 5. Doc convention

Every Phase C-N has three artifacts:

```
docs/17_phase_c_overview.md             ← this file
docs/c1/                                  ← per-phase folder
   design.md                              ← what + why + invariants
   impl_notes.md                          ← decisions taken during code
   test.md                                ← test plan + results
docs/c2/
   ...
```

Code lives out-of-tree as patches:

```
patches/openjdk-jdk17/
   0080-stopless-gc-skeleton.patch        ← C-1
   0081-stopless-gc-arguments.patch       ← C-2
   ...
```

Standalone C++ pieces (StoplessArena bridge, etc) live in:

```
src/cap_runtime/stopless_gc/              ← Phase A/B (done)
src/hotspot_bridge/                       ← any non-patch glue
```

## 6. Risks already known

* **shift=64 sbfm/ubfm** (Phase 1 残留): blocks `java -version`.
  Critical path again — was paused at end of Phase 1.
* **C2 JIT disabled** (clang ICE): we live with C1 + interpreter
  only. Acceptable for paper benchmarks.
* **Root scanning on C64**: oop on stack is 16B cap, not 8B word.
  frame::oops_do may need a purecap path.
* **JNI native methods**: native code holds non-cap pointers; we
  must NOT move objects reachable only through them, or must pin.
  v1: pin all objects with JNI refs.
* **CompressedOops**: must stay off (already known from Phase 1).
* **TLAB**: v1 uses a global allocator lock; profile before adding
  TLAB. May not need it if allocator is fast.

## 7. Non-goals (explicit cuts for v1)

* No generational separation
* No compaction beyond continuous moving
* No object pinning beyond JNI necessity
* No precise C2 JIT integration
* No NUMA awareness
* No multi-arena parallelism
* No remembered sets

## 8. Cross-references

* Paper-level design: `docs/15_cheri_stopless_design.md`
* Caprev runtime status: `docs/16_caprev_status.md`
* Phase 1 results: `docs/08_phase_1_results.md`
* shift=64 handoff: `docs/14_handoff_shift64_and_pivot.md`
* C2 status: `docs/07_c2_jit_status.md`

## 9. Verification helpers

* `scripts/c_phase_verify.sh` — staged verify of C-1..C-4 on hasee.
  Stage A: cap_runtime tests in QEMU.
  Stage B: openjdk patch dry-run.
  Stage C: hotspot build with `+UseStoplessGC`.
  Stage D: `java -XX:+UseStoplessGC -version`.
  Usage: `scripts/c_phase_verify.sh [--only A,B] [--keep-going]`.
