# SIMD codegen wordSize misuse — second-order seam after patch 0068

**Date:** 2026-05-27
**Status:** Identified, not fixed
**Severity:** Blocks the next 100+ classes from loading after 0068

## Context

After patch 0068 (ObjectAlignmentInBytes auto-bump) unblocked bootclass
loading from 13 to 146, the next blocker is the SIMD post-index
immediate guarantee in `assembler_aarch64.hpp:2295`:

```cpp
guarantee(imm == expectedImmediate, "bad offset");
```

Where `expectedImmediate = SIMD_Size_in_bytes[T] * regs` (e.g., for
T2D × 4 regs = 16×4 = 64 bytes per group).

Caller passes `imm=128` (= 2×64). Demoting this assert to warn (patch
0058 re-enabled) shifts the crash to the inner field encoder at
`assembler_aarch64.hpp:248`:

```cpp
guarantee(val < (1ULL << nbits), "Field too big for insn");
```

Where val=0x40 (=64) needs to fit in nbits=6 → 64 doesn't fit in 6
bits → guarantee fails. Demoting THIS too with truncation produced
broken codegen and JVM still crashed silently after ~146 classes.

## Root cause hypothesis

The same wordSize-as-8-bytes assumption that patch 0059 partially
addressed. Several sites in HotSpot aarch64 codegen use `wordSize` as
"size of a Java stack slot in bytes", which is 8 on standard LP64 but
**16 on CHERI**. Patch 0059 fixed seven sites in macroAssembler +
c1_Runtime1; more sites remain.

### Specific candidate

`macroAssembler_aarch64.cpp:2335`:
```cpp
ldpq(as_FloatRegister(regs[0]), as_FloatRegister(regs[1]),
     Address(post(stack, push_slots * wordSize * 2)));
```

`ldpq` loads a pair of 128-bit Q registers (32 bytes total). The
intent is `push_slots * sizeof(Q-pair)`. Standard LP64:
`push_slots * 8 * 2 = push_slots * 16` — which is per-Q stride, NOT
per-pair. Hmm, this might be slot-based not pair-based. Needs read.

In any case, `wordSize` here is being used as "Java stack slot
granularity" and breaks when wordSize != 8.

Also `macroAssembler_aarch64.cpp:2214`:
```cpp
Address(post(stack, count * wordSize))
```
Likely similar.

## Other "stp/ldp r0,r1, ..., 2*wordSize" sites

`grep -n "ldp\\|stp" -A 0 src/hotspot/cpu/aarch64/{interp_masm,macroAssembler,c1_Runtime1}_aarch64.cpp`
returns ~30 sites using `2 * wordSize` for integer-register stack pair
operations. On standard LP64, this is 16 bytes (correct for stp r0,r1).
On CHERI, it becomes 32 bytes — which **overshoots the 16 bytes
actually used by r0/r1 (8 bytes each)**.

Effect: pre-decrement subtracts 32 instead of 16 → stack frame uses
2× more space than declared. Post-increment likewise. Stack still
works (overestimate is safe; you just waste 16 bytes per
push/pop pair). But the wasted space might cause cap bounds violations
downstream if the stack region was cap-bounded.

## Why standard LP64 was OK

`wordSize = sizeof(intptr_t) = 8` on standard LP64. `BytesPerLong = 8`.
**They're equal.** All these uses of `wordSize` for "8-byte slot size"
silently work.

On CHERI: `wordSize = sizeof(intptr_t) = 16`. `BytesPerLong = 8`. They
**diverge**. Every site that meant "8-byte slot size" but wrote
`wordSize` is now wrong by 2x.

## The right fix (next patch)

Audit all aarch64 codegen for `wordSize` usage. Categorise each as:
- **machine word size** (= sizeof(intptr_t), 8 on LP64, 16 on CHERI):
  keep as wordSize
- **Java stack slot size** (always 8 bytes): replace with
  `BytesPerLong` or new constant `BytesPerJavaSlot`
- **Capability size** (always 16 bytes on CHERI; non-existent on LP64):
  introduce `BytesPerCap = sizeof(intptr_t)` and use that

Estimated scope: 30-50 sites across:
- `cpu/aarch64/interp_masm_aarch64.cpp`
- `cpu/aarch64/macroAssembler_aarch64.cpp`
- `cpu/aarch64/templateInterpreterGenerator_aarch64.cpp`
- `cpu/aarch64/stubGenerator_aarch64.cpp`
- `cpu/aarch64/c1_Runtime1_aarch64.cpp`
- `cpu/aarch64/templateTable_aarch64.cpp`

## Current actions

- Reverted the f() field overflow demote — too risky to ship (silently
  truncates instructions, produces broken code)
- Re-enabled patch 0058 (SIMD outer assert → warn) — provides
  diagnostic without unsafe truncation

## Findings preserved for next session

- imm=128, T=7 (T2D), regs=4 — expected imm = 64
- f() field overflow: val=0x40 in 6-bit field at lsb=10 or lsb=16
- Crash happens silently after ~146 bootclasses, no hs_err written
- The warning pattern shows 7+ distinct call sites tripping the SIMD
  imm assert

## Pairing with scan results

The docs/10 seam scan didn't directly catch these because they pattern-
match `wordSize * N` not `BytesPerWord * N`. Adding `wordSize \*` as a
Category I pattern to the scanner would surface 30+ more sites.
