# C-6 breakthrough — JVM now reaches String.<clinit>

**Date:** 2026-05-28 (continued from `docs/23_c6_resume_snapshot.md`)
**Status:** 4 bugs fixed in one session; JVM init reaches the first
Java method execution (`java.lang.String::<clinit>`) before hitting
the next blocker.

This file is the **resume packet** for whatever is hanging inside
`String.<clinit>`. It supersedes the "must not do" hypotheses in
docs/23 §"Hypotheses" — those guesses were wrong (the bug was much
deeper). Read this first.

## Bugs fixed this session (4 distinct, layered)

Each unblocked the next. None obvious from prior docs.

### Bug 1 — `STOPLESS_RUNTIME_DIR` path wrong → cap_runtime headers not found

`make/hotspot/lib/JvmFeatures.gmk:157` had
`STOPLESS_RUNTIME_DIR ?= $(TOPDIR)/../cap_runtime/stopless_gc`, but
our cap_runtime lives at `src/cap_runtime/`, two levels up from
openjdk's TOPDIR. The `wildcard libstopless_gc.a` check silently
failed, so `-I$(STOPLESS_RUNTIME_DIR)` was never added, and
`stoplessArena.cpp` failed at `fatal error: 'revoke.h' file not
found`.

**Fix:** `$(TOPDIR)/../../src/cap_runtime/stopless_gc` (committed in
patch 0082 yesterday, `dbd2577`).

### Bug 2 — `api.h` is aspirational

`cap_runtime/stopless_gc/api.h` declares 5 functions
(`stopless_init`, `stopless_arena_create`, etc.) that have no `.c`
body. Mac-session 0083 included api.h and called these → link-fail.

**Fix:** new 0083 includes `revoke.h` + `allocator.h` + `handler.h`
directly, never api.h (committed in `dbd2577`).

### Bug 3 — `PROT_MAX(PROT_READ|PROT_WRITE)` strips cap perms

CheriBSD's `mman.h` defines `PROT_CAP = 0x08` — without this bit in
the mmap prot flags, the returned cap lacks `STORE_CAP` permission.
`test_basic` and `test_alloc` passed because they never store caps
INTO the arena (they only check returned cap perms / use stack-local
slots). The JVM's first oop-store-into-arena happens during static
initialization of `java.lang.Throwable::NULL_CAUSE_MESSAGE` —
SIGPROT.

**Fix:** add `PROT_CAP` to both prot and `PROT_MAX` in
`stopless_arena_init` mmap call. After the fix, runtime check
confirms `cheri_perms_get(arena_base)` includes `W` (STORE_CAP).

### Bug 4 — `+UseCompressedOops` truncates caps to 32-bit narrow oops

Even after PROT_CAP, JVM still SIGPROT'd at Throwable init. Symptom
was `set_value(string, buffer); ... buffer = value(string)` returning
a cap with **`tag=0`** — the cap had been silently truncated by
storage into a 32-bit narrow-oop slot. `-XX:-UseCompressedClassPointers`
only disables CompressedClassPointers, NOT CompressedOops; the
launcher default has CompressedOops on. With 16-byte CHERI caps,
storage into a 4-byte narrow-oop field clears the tag, and subsequent
loads through that "address" SIGPROT.

**Fix:** must pass **both** `-XX:-UseCompressedClassPointers` **and**
`-XX:-UseCompressedOops` for any -version invocation. This is now a
permanent CLI requirement for the StoplessGC JVM until/unless we
either (a) implement a 64-bit narrow-oop encoding that preserves
caps, or (b) make HotSpot purecap-aware to refuse `-XX:+UseCompressedOops`
silently. Probably (a) for the paper; (b) for safety. Track in C-13.

### Bug 5 — `generate_string_indexof_stubs` shift=64 in mov immediate

The actual docs/14 shift=64 site, now precisely located via dladdr+addr2line
on `__builtin_return_address(0..3)` from inside the `Assembler::f()`
guarantee:

```
[C6f] OVERFLOW val=64 msb=15 lsb=10 nbits=6
[C6f]   ra0=0x41a17... (immediate caller inline)
[C6f]   ra1=0x4203e909 MacroAssembler::sub(Reg,Reg,uint64_t)
          → macroAssembler_aarch64.hpp:1187
[C6f]   ra2=0x41fff38d StubGenerator::generate_string_indexof_stubs
          → stubGenerator_aarch64.cpp:5703
[C6f]   ra3=0x41fff139 StubGenerator constructor
          → stubGenerator_aarch64.cpp:0
```

Root cause: `generate_string_indexof_linear()` emits a `sub` with
a 64-bit immediate that overflows the 12-bit add/sub-immediate field;
`wrap_adds_subs_imm_insn` falls back to `mov(Rd, imm) + sub-reg`; the
`mov(Rd, imm)` path encodes via bfm-family logical-immediate, whose
`imms` field at bits [15:10] is only 6 bits and overflows for our
generated immediate.

**Fix (v1):** under `#ifdef __CHERI_PURE_CAPABILITY__`, skip the
three string_indexof stub emissions; JIT/interpreter falls back to
the Java `String.indexOf` implementation. Correct but unoptimized.
(committed below as patch 0086.)

**Fix (v2, future):** make `mov(Rd, imm)` purecap-aware so it doesn't
emit a bfm with an out-of-range `imms`. Likely requires understanding
why the constant being moved is ≥ 64 bits' worth of bits to encode.

## Where we are now

```
[C6T] after init_globals               ← was crashing here before
[C6T] before VMThread::create
[C6T] after VMThread::create
[C6T] after os::create_thread(vmthread)
[C6T] vmthread active_handles ready
[C6T] VMThread fully started
[C6T] before initialize_java_lang_classes
[C6IJC] IJC enter
[C6IJC] before init_class String
[10.766s] Initializing 'java/lang/Object'(no method)     ← Object: no <clinit>
[10.773s] Initializing 'java/lang/CharSequence'(no method)  ← interface
[10.777s] Initializing 'java/lang/String'                ← THE FIRST Java method
       ← hangs here forever (10+ minutes, no progress)
```

## The new blocker: String.<clinit> hangs

The very first Java method to actually execute (`java.lang.String::<clinit>`)
hangs the JVM. Same behavior with `-Xint` (force interpreter), so it's
not JIT. Same behavior with EpsilonGC (so not StoplessGC-specific).

Hypotheses to explore next session:

1. **The interpreter has a bfm-family shift=64 in its dispatch table.**
   Same root cause as bug 5 but in `templateInterpreter_aarch64.cpp`
   generation. Diagnostic: check whether any `[C6f] OVERFLOW`
   triggers during interpreter generation (it should have during
   `stubRoutines_init1` or `_init2`, but only string_indexof hit it
   so far — maybe interpreter dispatch is generated lazily).

2. **`String.<clinit>` calls an intrinsic stub that's `nullptr`.**
   Our v1 fix set 3 indexof stubs to nullptr; if Java code calls
   them through the intrinsics table (`vmIntrinsics::_indexOfLB1`
   etc.) without checking, dereferencing nullptr could either crash
   (we'd see SIGPROT) or hang (if it lands at address 0 which on
   CHERI is bounds-zero cap → silent loop). Diagnostic:
   `grep _string_indexof_linear` to find call sites; add a guard or
   provide a stub that just `ret` (does nothing, returning -1).

3. **Class-init recursion deadlock.** `String.<clinit>` may try to
   load a class that depends on String. The init monitor on String
   blocks until String.<clinit> completes, but the recursion needs
   String already-initialized. Diagnostic: `-Xlog:class+init=trace`.

4. **The JavaCalls::call_static infrastructure on CHERI has an issue.**
   First-time Java call requires the interpreter's call_stub (from
   StubRoutines::call_stub) which docs/14 already implicated.
   Possible the call_stub still has a subtle issue post-0068-0070.

Most likely: (2). The intrinsics table.

## Repro

```bash
# On hasee:
LIBJVM=$HOME/projs/stopless-java-gc/third_party/openjdk-jdk17/build/jdk-morello/jdk/lib/server/libjvm.so
STRIP=$HOME/projs/stopless-java-gc/third_party/output/morello-sdk/bin/aarch64-unknown-freebsd-strip
cp "$LIBJVM" /tmp/libjvm.so && $STRIP /tmp/libjvm.so
scp -P 10005 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    /tmp/libjvm.so root@localhost:/opt/jdk/lib/server/libjvm.so

# CRITICAL FLAGS: must include both -XX:-UseCompressedClassPointers AND
# -XX:-UseCompressedOops or you'll hit bug 4 (different crash earlier).
ssh -p 10005 root@localhost \
    '/opt/jdk/bin/java -XX:+UnlockExperimentalVMOptions -XX:+UseStoplessGC \
     -Xms16m -Xmx32m \
     -XX:-UseCompressedClassPointers -XX:-UseCompressedOops \
     -Xlog:class+init=info \
     -version'
# Expected: hangs after "Initializing 'java/lang/String'"
```

## Things to commit from this session

Real fixes (will go into patch 0086):
- `make/hotspot/src/hotspot/cpu/aarch64/stubGenerator_aarch64.cpp`:
  add `#ifdef __CHERI_PURE_CAPABILITY__` guard around
  `generate_string_indexof_stubs` body.
- `src/cap_runtime/stopless_gc/revoke.c`: add `PROT_CAP` to mmap.

Diagnostic instrumentation (NOT to commit; live in working tree as
debug fprintfs):
- assembler_aarch64.hpp f() with __builtin_return_address(0..3) + dladdr
- init.cpp init_globals breadcrumbs
- universe.cpp Universe::genesis breadcrumbs
- systemDictionary.cpp SD::initialize + resolve_or_fail breadcrumbs
- vmClasses.cpp resolve_until + per-class-id prints
- klassFactory.cpp create_from_stream breadcrumbs
- classFileParser.cpp create_instance_klass + fill_instance_klass
- javaClasses.cpp create_mirror + initialize_mirror_fields +
  initialize_static_field + initialize_static_string_field +
  basic_create + create_from_unicode
- constantPool.cpp uncached_string_at
- thread.cpp Threads::create_vm + initialize_java_lang_classes
- allocator.c per-alloc print
- classLoader.cpp load_class breadcrumbs

These are kept in the working tree because the next session will
need them to drill into String.<clinit>. Strip them when C-6 is fully
resolved.

## Task tracker delta

```
C-6 (shift=64): 🟡 → 🟢 mostly done — 5 root-causes found and fixed.
                Remaining: String.<clinit> hang (likely a 6th root cause
                in same family). New tracker line:
C-6b (clinit hang): hang on first Java method execution; pinpointed but
                    not yet root-caused. Resume from §"Hypotheses" above.
```
