# C-6 Resume Snapshot — shift=64 hunt, post-Genesis crash

**Date:** 2026-05-28 (early morning, post-midnight handoff)
**Last commit before this snapshot:** see `docs/22_phase_task_tracker.md` for
prior state; today's commits land C-3 + C-4 + this doc.
**Predecessor:** `docs/14_handoff_shift64_and_pivot.md` (the original
shift=64 pause doc, ~2026-05-23).

This file is the **C-6 resume packet** — read this first if you're
picking up shift=64 work after a session break.

## TL;DR — what changed this session

1. **C-3 + C-4 landed** as a single canonical patch
   `patches/openjdk-jdk17/0083-stopless-arena-and-allocate.patch`.
   `libjvm.so` rebuilt clean (32 grep hits for "Error|error:" were all
   `-Wcheri-provenance` warnings + `safefetch` linker warnings, none
   from new code).
2. **`api.h` is aspirational** — declares `stopless_init`,
   `stopless_arena_create/destroy`, `stopless_move`,
   `stopless_revoke_sweep`, none of which have `.c` bodies. Real API
   surface = `revoke.h` + `allocator.h` + `handler.h`. The Mac session
   wrote the patch against `api.h` → linker errors. **Do not include
   api.h from JVM-side wrappers; use the lower-level headers.**
3. **JvmFeatures.gmk path was wrong** —
   `STOPLESS_RUNTIME_DIR ?= $(TOPDIR)/../cap_runtime/stopless_gc`
   silently fails the `wildcard libstopless_gc.a` check (path doesn't
   exist; cap_runtime is at `src/cap_runtime/`, not
   `third_party/cap_runtime/`). Fixed in new `0082`:
   `$(TOPDIR)/../../src/cap_runtime/stopless_gc`.
4. **C-6 crash point moved** vs `docs/14`. New crash location is
   **post-Genesis**, in init code that runs between Genesis completion
   and any of (`StubRoutines::initialize2`, class init, vmthread start,
   JVMTI init).
5. **The crash is GC-agnostic** — see "The smoking gun" below.

## The smoking gun: same crash for Epsilon and Stopless

Verified empirically with these two runs on QEMU CheriBSD purecap:

### StoplessGC trace (SIGPROT)
```
[0.467s][info][os,thread]   Thread attached
[0.671s][info][startuptime] StubRoutines generation 1, 0.0144 secs
[1.052s][info][startuptime] Genesis, 0.3226 secs
                            ← In-address space security exception
                            ← exit=162 (= 128 + SIGPROT 34)
```

### EpsilonGC trace (hang)
```
[0.363s][info][os,thread]   Thread attached
[0.543s][info][startuptime] StubRoutines generation 1, 0.0881 secs
[0.607-0.969s][info][gc,init] (heap config prints)
[1.060-1.069s][info][gc,init] TLAB Size Max / Elasticity / Decay
[1.400s][info][startuptime] Genesis, 0.7967 secs
                            ← hangs forever; killed at timeout=90s
```

**Both** clear Genesis and crash IMMEDIATELY after, in the very next
init phase.

### Why different surfaces (hang vs SIGPROT)?

* **StoplessGC** doesn't yet install a SIGPROT handler
  (`stopless_handler_install` is deferred to C-7). The bad capability
  access kills the process cleanly via the FreeBSD kernel default
  action ("In-address space security exception" + signal-34 exit).
* **EpsilonGC** runs with HotSpot's standard signal handler already
  registered. The bad-cap-access triggers signal-34 → HotSpot's
  handler tries to write `hs_err_pid*.log` → that handler itself does
  bad-cap-accesses → recursive signal handling → effective deadlock.
  Symptom: hang, no hs_err produced.

The implication: **fix shift=64 once, and BOTH GCs proceed**. This is
the lever for paper §3 + §4 evaluation.

### Why this matters for the paper

Section 3 (Porting Experience) gets a non-trivial datapoint: HotSpot's
crash-handler-on-CHERI deadlock is a research-grade finding — it
demonstrates that the standard hs_err path is itself
capability-unsafe in early init. Mention as future work or as a
constraint on the porting methodology.

## Repro command (copy-paste, on hasee)

```bash
cd /home/bc/projs/stopless-java-gc

# Strip and ship to QEMU
STRIP=$HOME/projs/stopless-java-gc/third_party/output/morello-sdk/bin/aarch64-unknown-freebsd-strip
LIBJVM=$HOME/projs/stopless-java-gc/third_party/openjdk-jdk17/build/jdk-morello/jdk/lib/server/libjvm.so
cp "$LIBJVM" /tmp/libjvm.so && $STRIP /tmp/libjvm.so
scp -P 10005 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    /tmp/libjvm.so root@localhost:/opt/jdk/lib/server/libjvm.so

# Trace one or the other
QSSH='ssh -p 10005 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@localhost'

# StoplessGC — fast crash (~1.1s + SIGPROT)
$QSSH '/opt/jdk/bin/java -XX:+UnlockExperimentalVMOptions -XX:+UseStoplessGC \
       -Xms8m -Xmx32m -XX:-UseCompressedClassPointers \
       -Xlog:startuptime,os+thread=info,gc+init -version'

# EpsilonGC — slow hang (~1.4s + freeze; kill with timeout 90)
timeout 90 $QSSH '/opt/jdk/bin/java -XX:+UnlockExperimentalVMOptions -XX:+UseEpsilonGC \
       -Xms8m -Xmx32m -XX:-UseCompressedClassPointers \
       -Xlog:startuptime,os+thread=info,gc+init -version'
```

A full iteration (edit source → rebuild → ship → trace) is ~10–15 min
locally on hasee. Most of that is the OpenJDK rebuild.

## Hypotheses for the post-Genesis crash site

In `src/hotspot/share/runtime/init.cpp::init_globals()`, after
`universe2_init()` (= Genesis), the next calls are roughly:

```cpp
universe2_init();              // ← "Genesis" startuptime print ENDS here
interpreter_init_stub();
invocationCounter_init();
accessFlags_init();
InstanceKlass::compute_default_layout();   //   <-- bitfield code; SUSPECT 1
javaClasses_init();                         //   <-- SUSPECT 2
SystemDictionary::initialize();
gc_barrier_stubs_init();        // calls StubRoutines::initialize2() too?
universe_post_module_init();
```

Top suspects, ordered by plausibility:

1. **`accessFlags_init` / `InstanceKlass::compute_default_layout`** —
   touches `OopMapBlock`, klass field layout. Field-width calculations
   use `BitsPerWord` (= 64 on CHERI same as LP64 because Bits-per-word
   is logically separate from cap size). If any code does
   `bfi(rd, src, lsb, width)` with `width = BitsPerWord - something`
   that becomes 64+, the `imms` field overflows.

2. **`javaClasses_init`** — `java_lang_Class::compute_offsets()`.
   Reads Klass cap-pointer offsets which patches 0066-0067 already
   touched. May have a residual wordSize/HeapWordSize confusion
   surfaced now that we're past it.

3. **A second stub-generation phase**. Patch `0069` fixed one specific
   SIMD bytesPerLong site; possibly another site exists in stubs that
   `StubRoutines::initialize2()` generates.

## Suggested next-iter instrument (per docs/14 §"resume points")

The one tactic NOT tried in docs/14:

> "register signal(SIGABRT, ...) and inside the handler walk
> caller-via-getcontext-ucontext. That gives the real PC + register
> state at trap."

Plus a NEW tactic enabled by today's GC-agnostic discovery:

> Compare EpsilonGC's deadlock state vs StoplessGC's prompt SIGPROT to
> isolate which non-GC init function is crashing. Use `gdb -p
> $(pidof java)` ON THE QEMU GUEST after the EpsilonGC freeze:
> `bt` should give the recursive crash-handler stack, with the
> ORIGINAL faulting PC near the bottom.

GDB-on-CheriBSD-purecap may have its own issues, but worth one shot.

## What MUST NOT be done

(From this session's experience.)

1. **Don't include `cap_runtime/stopless_gc/api.h`** from any JVM-side
   C++ TU. It declares 5 functions that have no `.c` body and your
   build will link-fail. Use `revoke.h` + `allocator.h` + `handler.h`
   directly via `extern "C"`.
2. **Don't hand-author patches under `patches/openjdk-jdk17/` against
   a tree you can't see** (Mac session pitfall). Write the final
   source state via `Edit`, then `git diff` it. The exception
   is true one-liner patches with a uniquely-named anchor (like
   `0082`'s `STOPLESS_RUNTIME_DIR` line).
3. **Don't split a regen into multiple patches if they touch the same
   files in sequence** (old 0083 + 0085 trap). Merge them. See this
   session's `0083-stopless-arena-and-allocate.patch`.
4. **Don't trust `docs/14`'s `generate_call_stub` claim** as the
   current crash site — that bug was fixed by 0068-0070. The bug now
   lives post-Genesis.

## Files touched today (2026-05-27 → 2026-05-28)

Repo (committed):

```
patches/openjdk-jdk17/
  0082-stopless-runtime-link.patch          [replaced from no-op stub]
  0083-stopless-arena-and-allocate.patch    [NEW; supersedes old
                                              0083+0085, kept as .orig]
  0083-stopless-arena-cpp-bridge.patch.orig [archived hand-authored]
  0085-stopless-arena-allocate-wire.patch.orig [archived hand-authored]
scripts/
  c_phase_verify.sh                          [Stage B patch list updated]
docs/
  23_c6_resume_snapshot.md                   [THIS FILE]
```

OpenJDK tree (gitignored; effective state via .stopless/patch-state):

```
applied patches now include:
  0080-stopless-gc-skeleton.patch
  0081-stopless-gc-feature-enable.patch
  0082-stopless-runtime-link.patch
  0083-stopless-arena-and-allocate.patch
```

`libjvm.so` rebuilt and shipped to QEMU at `/opt/jdk/lib/server/`.

## Task-tracker state at handoff

| # | Phase | Status |
|---|---|---|
| C-1 | HotSpot skeleton | ✅ committed |
| C-2 | -XX:+UseStoplessGC parse | ✅ committed |
| C-3 | StoplessArena C++ wrapper | ✅ this session |
| C-4 | bump-pointer allocate wire | ✅ this session |
| C-5 | java -XX:+UseStoplessGC -version exit 0 | ⬜ blocked on C-6 |
| **C-6** | **post-Genesis shift=64 fix** | 🟡 **in_progress** — this doc is the resume packet |
| C-7..C-12 | downstream | ⬜ blocked on C-5 |
