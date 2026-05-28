# C-6 session finale — 7 layers in one session

**Date:** 2026-05-28 (full-day session continuation of docs/24 + docs/25)
**Iterations:** 32 build + ship + run cycles in QEMU CheriBSD purecap
**Commits this session:** 7 (dbd2577 → 2498339)
**Status:** C-6 is no longer "the shift=64 bug" — it's a **cascading
CHERI-port long tail** that this session has documented and partially
fixed. JVM now reaches `java.lang.String::<clinit>` execution before
the next blocker.

## The 7 layers, in order

Each fix exposed the next. None of them was obvious from prior docs.
This is the kind of "Porting Experience" content the paper §3 wants.

### Layer 1 — make-system path bug
`STOPLESS_RUNTIME_DIR` pointed to a non-existent directory, so
`-I` never got added, so `revoke.h` wasn't found, so build failed.

### Layer 2 — `api.h` is aspirational
5 functions declared, none implemented. Mac session's old 0083
used them → link-fail.

### Layer 3 — `PROT_MAX(R|W)` strips cap perms
mmap'd arena had `STORE_CAP` perm missing, so first oop-store
SIGPROT'd. test_basic/test_alloc accidentally never exercised this.
Fix: add `PROT_CAP` to mmap.

### Layer 4 — `+UseCompressedOops` truncates caps
`-XX:-UseCompressedClassPointers` only handles klass pointers, NOT
oops. With CompressedOops on, 16-byte CHERI caps get stored as
4-byte narrow oops, clearing the tag. Both `-XX:-UseCompressedOops`
**and** `-XX:-UseCompressedClassPointers` are now permanent CLI
requirements. Track as future C-13.

### Layer 5 — `generate_string_indexof_stubs` shift=64
Located via `dladdr + addr2line + __builtin_return_address(0..3)`
from inside `assembler_aarch64.hpp f()` guarantee.
`MacroAssembler::sub(Reg, Reg, uint64_t)` → `wrap_adds_subs_imm_insn`
→ fallback `mov(Rd, imm) + sub-reg`, where the `mov` path encodes
via bfm with `imms[15:10]` overflow. v1 fix: skip these stubs on
CHERI (patch 0086). JIT falls back to Java String.indexOf.

### Layer 6 — `mov(Reg, Reg)` is integer ORR
The classic `orr Rd, ZR, Rn` is INTEGER and clears Rn's cap tag.
`StubRoutines::forward_exception` did `__ mov(r19, r0)` to move the
exception handler cap, then `__ br(r19)` to dispatch. The mov
cleared the tag, the br landed at tag-zero PC, SIGPROT.
v1 fix: use `add Rd, Rn, #0` instead (patch 0088). **But see
"Open: Layer 6 is incomplete" below.**

### Layer 7 — `mov(Rd, zr)` overflow from above fix
Patch 0088 broke `mov(Rd, zr)` because HotSpot encodes `zr` as
Register #32 (distinct from r31_sp=#31 and sp=#33). `Assembler::add`
doesn't translate #32 → val=32 leaks into the 5-bit Rn field →
assembler guarantee. Specifically tripped in
`templateInterpreterGenerator_aarch64.cpp:1145`.
Fix: special-case `Rn == zr` in mov(Reg, Reg) to fall back to orr
(patch 0089). Semantically correct because mov-from-zr means
"zero out Rd" — tag-clearing is intended.

## Open: Layer 6 is incomplete (next-session work)

After patches 0088+0089 land, the JVM progresses past
generate_native_entry and into `String::<clinit>`. But the SIGPROT
at `forward_exception` END (PC = 0x428b4138 in latest build,
0x428b8138 in earlier) **still fires**. Same fault signature: tag-0
PC at the end of forward_exception.

Why didn't patch 0088 fix this? Because **`Assembler::add(Rd, Rn, #0)`
on Morello is still INTEGER ADD** — it doesn't preserve cap tags.
The actual Morello cap-preserving "move via add" is a DIFFERENT
opcode (`add Cd, Cn, #imm`), not the standard AArch64 ADD.

Patch 0088 was wrong about the fix mechanism. The diagnostic
sequence (PC at end-of-stub, register lost cap tag) was right; the
fix just doesn't reach the right opcode.

### What the real fix needs

The HotSpot AArch64 assembler needs a true "Morello cap-mov"
primitive. Options:

1. **Add an `Assembler::cmov(Register Rd, Register Rn)`** that emits
   the Morello `add Cd, Cn, #0` opcode (or whatever the proper
   cap-preserving move encoding is). Requires reading Morello ARMv8.5-A
   ARM ARM for the cap-extension opcode space, then implementing it
   in assembler_aarch64.hpp/cpp.
2. **Find a pre-existing cap-mov helper** in the HotSpot CHERI port
   (if any earlier patch added one). grep for `__intcap`, `capability`,
   `cap_mov` in macroAssembler_aarch64.{hpp,cpp}.
3. **Inline-asm fallback** for the critical sites (forward_exception's
   line 509). Use `__asm__ volatile("...":::)` to emit the cap-mov
   instruction directly, bypassing the assembler abstraction.

Recommend option 1; option 3 as quick unblock.

## Summary of commits

```
dbd2577  Phase C-3 + C-4 patches landed (StoplessArena + bump-allocator)
f86acc5  docs/23: C-6 resume snapshot — post-Genesis crash, GC-agnostic
dcb0980  Phase C-6 progress: 4 root causes fixed, JVM reaches first Java method
dabe562  Phase C-7 partial + docs/25: SIGPROT now diagnoseable, PC pinpointed
a52e56c  Phase C-6 fix #6: cap-preserving mov(Reg, Reg) on CHERI (incomplete)
2498339  Phase C-6 fix #7: mov(Rd, zr) needs orr fallback (patch 0088 follow-up)
+ this docs/26
```

## Diagnostic instrumentation kept in WT (NOT committed)

Long list of `fprintf(stderr, "[C6X] ...")` calls scattered through:

- `assembler_aarch64.hpp` (f() with insn_so_far + dladdr backtrace)
- `init.cpp` (init_globals breadcrumbs)
- `universe.cpp` (Universe::genesis breadcrumbs)
- `systemDictionary.cpp` (SD::initialize)
- `vmClasses.cpp` (resolve_until + per-class-id prints)
- `klassFactory.cpp` (create_from_stream)
- `classFileParser.cpp` (create_instance_klass + fill_instance_klass)
- `javaClasses.cpp` (create_mirror + initialize_mirror_fields +
  initialize_static_field + initialize_static_string_field +
  basic_create + create_from_unicode)
- `constantPool.cpp` (uncached_string_at)
- `thread.cpp` (Threads::create_vm + initialize_java_lang_classes)
- `allocator.c` (per-alloc print)
- `classLoader.cpp` (load_class)
- `stoplessHeap.cpp` (SIGPROT install message — already in 0087)

Keep these in WT for next session to pick up the trail. Strip at C-6
final closure.

## What the paper §3 gets out of this

Real "Porting Experience" content, each as a paragraph:

1. CheriBSD `PROT_CAP` is not the default for `MAP_ANON` mmap and
   isn't covered by typical `PROT_MAX(R|W)`. Subtle silent
   incomplete-permission failure.
2. `-XX:+UseCompressedOops` is a silent-kill in CHERI purecap — the
   JVM should refuse this combination at startup. (We have to manually
   pass `-XX:-UseCompressedOops`.) Defensive: a single one-line CHERI
   check in `Arguments::apply_settings`. Saves future ports a day.
3. `String.<clinit>` is the canonical "first Java code executed"
   stress test for a JVM port — drives the largest path through
   stubs, interpreter, class init, and mirror access. A useful
   gating point for incremental porting work.
4. **The integer-ORR-vs-cap-MOV register-move pitfall**: every
   r-r move in HotSpot quietly drops cap tags unless the assembler
   knows about Morello's cap-mov idiom. This is the kind of subtle
   ISA mismatch that doesn't show up in any compiler error.

## Repro

```bash
# On hasee. Critical flags: BOTH CompressedOops/ClassPointers off.
ssh -p 10005 root@localhost \
  '/opt/jdk/bin/java -XX:+UnlockExperimentalVMOptions -XX:+UseStoplessGC \
     -Xms16m -Xmx32m \
     -XX:-UseCompressedClassPointers -XX:-UseCompressedOops \
     -Xlog:class+init=info \
     -version'

# Expected output:
#   [Stopless] SIGPROT handler installed
#   ... (init progresses to)
#   [N.NNNs] Initializing 'java/lang/String'
#   [stopless] unforwarded fault: pc=0x428b4138 ...
#   exit=162
#
# pc=0x428b4138 = END of StubRoutines::forward_exception (PrintStubCode
# confirms). br r19 lands at tag-0 PC because mov(r19, r0) is still
# integer add. Layer 6 incomplete; see "What the real fix needs" above.
```
