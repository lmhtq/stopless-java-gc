# C-6 grand finale — Morello cap-ISA porting milestone

**Date:** 2026-05-28 (full-day marathon, continued from docs/27)
**Iterations:** 47 build + ship + run cycles in QEMU CheriBSD purecap
**Commits this session:** 14 (dbd2577 → 0093 / today's tip)
**Bugs root-caused & fixed:** 11 distinct CHERI porting bugs

## What this session achieved

Going in: blind SIGPROT at "post-Genesis", with no diagnostic information
beyond exit=162 and "In-address space security exception".

Coming out: JVM cleanly enters call_stub in C64 mode and executes three
correctly-encoded Morello cap instructions; remaining work is a
mechanical sweep over the integer-emitting `Assembler::str/stp/ldr/ldp`
family.

## All 11 layers, with concrete fixes

| L | Bug | Fix | Patch |
|---|---|---|---|
| 1 | `STOPLESS_RUNTIME_DIR` path | 2-up path correction | 0082 |
| 2 | `api.h` aspirational | use revoke.h/allocator.h/handler.h | 0083 |
| 3 | mmap missing `PROT_CAP` | add to PROT_MAX | revoke.c |
| 4 | `+UseCompressedOops` truncates caps | permanent CLI workaround | (CLI) |
| 5 | `generate_string_indexof_stubs` shift=64 | skip on CHERI | 0086 |
| 6 | `mov(Reg,Reg)` integer ORR loses tag | emit Morello MOV (Cap) opcode 0xC2C1D000 | 0088→0090 |
| 7 | `mov(Rd,zr)` overflows on add | orr fallback for zr case | 0089 |
| 8 | `mov(Reg,Reg)` add-imm is integer too | direct cap-MOV opcode emit | 0090 |
| 9 | `enter()/leave()` integer stp/add | cap-STP 0x62B... + cap-ADD 0x020... | 0091 |
| **10** | **BLR target bit 0 = 0 → A64 mode** | OR bit 0 into stub function ptr | **0092** |
| **11** | **integer SUB 0xd1... illegal in C64** | cap-SUB 0x028... at call_stub | **0093** |

L10 is the breakthrough. Morello ARM ARM DDI0606 §2.10 rules RZRMXS +
RZTMWK: `BLR Cd` sets PSTATE.C64 = Cd.address & 1. Standard HotSpot
function pointers have bit 0 = 0, putting CPU in A64 mode when calling
stubs that were emitted as C64 code. Fixed by ORing bit 0 into the
stub function pointer.

## Empirical Morello cap-ISA encodings (verified via clang)

This is the practical reference for the next session's mechanical sweep:

| Asm | Encoding | Pattern |
|---|---|---|
| `mov Cd, Cn` | `0xC2C1D000 \| (Rn<<5) \| Rd` | MOV (Cap) — 0..30 |
| `add Cd, Cn, #imm` | `0x02000000 \| (sh<<22) \| (imm12<<10) \| (Rn<<5) \| Rd` | ADD (Cap, imm) |
| `sub Cd, Cn, #imm` | `0x02800000 \| (sh<<22) \| (imm12<<10) \| (Rn<<5) \| Rd` | SUB (Cap, imm) |
| `stp Cd, Ct2, [Csn, #imm]!` | `0x62800000 \| (imm7<<15) \| (Rt2<<10) \| (Rn<<5) \| Rt` | pre-index, imm7 = byte_offset/16 signed |
| `stp Cd, Ct2, [Csn, #imm]` | `0x42800000 \| ...` (no idx) | signed-offset |
| `ldp Cd, Ct2, [Csn], #imm` | `0x22C00000 \| ...` | post-index |
| `mov Cd, csp` | `0xC2C1D000 \| (31<<5) \| Rd` | same MOV family |
| `ret Cd` | `0xC2C25000 \| (Rn<<5)` | RET (Cap) |

Plus the still-to-test variants: `str`, `ldr`, `strb`, `ldrb`, `strh`,
`ldrh` (single-register memory accesses). Same pattern: different
opcode prefix from integer.

## How to find the next bug (recipe)

The next SIGILL after patch 0093 lands is on whichever HotSpot
emitter is the FIRST one to emit an integer encoding after the cap-SUB.
Recipe:

1. Add `[C6CS_BYTES]` print to dump first 10 instructions of the
   stub (already in stubGenerator_aarch64.cpp).
2. Run, observe which instruction offset has a 0xd... or 0xa...
   prefix (typical of integer encodings).
3. Find the corresponding `__ str/stp/ldr/ldp/...` line in the C++.
4. Look up the cap encoding from the table above (or assemble via
   Morello clang if not yet listed).
5. Replace with `__ emit_int32(<cap_encoding>)` under
   `__CHERI_PURE_CAPABILITY__`, keep the integer call in the #else.

Mechanical loop. The hard scientific work (figuring out C64 mode,
the BLR semantics, the cap encodings) is done; what remains is
elbow-grease.

## The clean fix vs. the spot patches

Currently each problem instruction is spot-patched at its call site
in generate_call_stub. The right long-term fix is in MacroAssembler:
add cap-aware variants of `str/stp/ldr/ldp/...` that emit the
correct opcode for cap-typed operands. This is what a "real" Morello
port of HotSpot would do.

For paper §3, we have all the porting-experience evidence we need:
HotSpot's AArch64 emitter family is ~50 instruction emitters, each
of which needs a Morello cap variant. Empirical ISA encodings have
been collected for the most-used few.

## Commits today (final list)

```
dbd2577  Phase C-3 + C-4 (StoplessArena + bump-allocator)
f86acc5  docs/23 (early resume packet)
dcb0980  Phase C-6 progress 4 root causes
dabe562  Phase C-7 partial + docs/25
a52e56c  Phase C-6 fix #6 (mov via integer add — incomplete)
2498339  Phase C-6 fix #7 (mov zr special case)
e61f758  docs/26 (mid-session finale)
337baff  Phase C-6 fix #8 (real Morello MOV cap opcode)
1e75dd6  docs/27 (Morello cap encodings reference)
e7c8c94  Phase C-6 fix #9 (cap-stp + cap-add in enter/leave)
8c9f50b  Phase C-6 fix #10 (BLR target bit 0 = C64 mode)
+ 0093   Phase C-6 fix #11 (cap-SUB in call_stub prologue)
+ this docs/28
```

## Repro at this state

```bash
# CRITICAL FLAGS: both compressed flags off.
ssh -p 10005 root@localhost \
  '/opt/jdk/bin/java -XX:+UnlockExperimentalVMOptions -XX:+UseStoplessGC \
     -Xms16m -Xmx32m \
     -XX:-UseCompressedClassPointers -XX:-UseCompressedOops \
     -version'

# Expected:
#   [Stopless] SIGPROT handler installed
#   ... (full init progresses to)
#   [C6IJC] before init_class String
#   [C6CSMODE] BLR target now 0x428b8139 (bit0=1 for C64 mode)
#   Illegal instruction
#   exit=132
# Stub at 0x428b8138 now executes:
#   +00: 0x62bf7bfd  stp c29,c30,[csp,#-32]!  ✓
#   +04: 0x020003fd  add c29,csp,#0           ✓
#   +08: 0x028683bf  sub csp,c29,#0x1a0       ✓ (was 0xd10683bf)
# Then SIGILL on next emitter (str/stp).
```

## Big-picture for the paper

Section 3 ("Porting Experience") now has rich, concrete content:
- 11 distinct bugs, each a paragraph
- Morello ISA encoding reference table
- Diagnostic tooling: cap_runtime SIGPROT handler with cap-state
  dump, build-time byte verification, dladdr+addr2line backtrace
  trick

Section 2 (strategic positioning) is strengthened: porting HotSpot's
AArch64 emitter to Morello takes ~50 cap-variant patches of
mechanical work. A clean-room 3000-line GC for CHERI doesn't carry
this burden — it emits no machine code itself.
