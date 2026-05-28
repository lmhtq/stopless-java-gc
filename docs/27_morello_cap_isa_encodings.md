# Morello CAP ISA encodings — empirical reference

**Date:** 2026-05-28 (end of session, supersedes docs/26's "needs ISA reference" note)
**Method:** assembled `mov/add/stp/ldp/ret` variants with the Morello-aware
clang at `third_party/output/morello-sdk/bin/clang` (target
`aarch64-unknown-freebsd -march=morello -mabi=purecap`) and disassembled
the resulting object to read the bytes back. Reproducible — see
`/tmp/mov_test.S` and `/tmp/stp_test.S` (preserve those test fixtures).

## Why this doc exists

C-6 turned out to be a CASCADING set of bugs of the form "HotSpot
emits an AArch64 *integer* instruction where Morello purecap needs
the *capability* variant". The integer instruction's bit pattern
is silently wrong on Morello — it executes successfully but performs
INTEGER arithmetic on registers that hold capabilities, clearing the
cap tag in the destination. Subsequent uses of that "cap" as a base
for cap-load/store/branch then SIGPROT with PROT_CHERI_TAG.

This isn't one bug. It's **every reg-reg/reg-imm instruction in the
HotSpot AArch64 assembler that touches a cap-typed value**. Each
emitter needs a CHERI-aware variant. This doc gives the empirical
encodings to base those variants on.

## Encoding pairs

For each pair: top row = the CAP variant (preserves tag); bottom row =
the INTEGER variant (clears tag of dest when source was a cap).

### Move (register → register)

| Asm | Hex | Notes |
|---|---|---|
| `mov c0, c1`     | `0xC2C1D020`   | **CAP — preserves tag**. Pattern: `0xC2C1D000 \| (Rn<<5) \| Rd` |
| `mov c19, c0`    | `0xC2C1D013`   | same |
| `mov c2, c3`     | `0xC2C1D062`   | same |
| `mov c29, csp`   | `0xC2C1D3FD`   | mov from csp — same encoding family |
| `mov x0, x1`     | `0xAA0103E0`   | integer (orr alias). Clears cap tag of x1. |

`mov(Reg, Reg)` in HotSpot's macroAssembler used to emit `orr Rd, ZR,
Rn` (integer). Patches 0088, 0089, 0090 in this repo replaced this
with the cap variant via `emit_int32(0xC2C1D000 | (Rn<<5) | Rd)` when
neither Rd nor Rn is zr/sp. The sp/zr special cases remain on integer
forms (semantically correct for those cases).

### Add immediate

| Asm | Hex | Notes |
|---|---|---|
| `add c0, c1, #0`   | `0x02000020` | **CAP** — preserves tag. Pattern: `0x02000000 \| (imm12<<10) \| (Rn<<5) \| Rd` |
| `add c29, csp, #0` | `0x020003FD` | cap-add from csp |
| `add x0, x1, #0`   | `0x91000020` | integer (sf=1) |

**Important:** `Assembler::add(Rd, Rn, imm)` in HotSpot today emits
the *integer* (`0x91xxxxxx`) variant. Patch 0088's initial attempt
used `Assembler::add(Rd, Rn, 0U)` as a substitute for cap-mov — that
was wrong because it still emits the integer encoding. Patch 0090
fixed mov to use the actual CAP-MOV `0xC2C1D000`, but `add` itself
is still integer everywhere it appears in HotSpot.

### Store pair, pre-index

| Asm | Hex | Notes |
|---|---|---|
| `stp c29, c30, [csp, #-32]!` | `0x62BF7BFD` | **CAP** — stores tagged 16-byte pair. `imm7` scaled by 16. |
| `stp x29, x30, [sp, #-16]!`  | `0xA9BE7BFD` | integer 8-byte pair. `imm7` scaled by 8. |

Pattern for cap-stp pre-index (from one sample, not yet generalized):
`0x62B00000 \| (imm7_scaled<<15) \| (Rt2<<10) \| (Rn<<5) \| Rt`
(imm7 is signed 7-bit, scaled by 16 for cap-pair).

### Store pair, simm-offset (no pre/post-index)

| Asm | Hex |
|---|---|
| `stp c0, c1, [csp]`          | `0x428007E0` |
| `stp c0, c1, [csp, #32]`     | `0x428107E0` |

Cap-stp signed-offset variant. The `#32` offset becomes imm7=2 (scaled
by 16). Pattern: `0x42800000 \| (imm7_scaled<<15) \| (Rt2<<10) \| (Rn<<5) \| Rt`.

### Load pair, post-index

| Asm | Hex |
|---|---|
| `ldp c29, c30, [csp], #32`   | `0x22C17BFD` |

Cap-ldp post-index. Pattern: `0x22C00000 \| (imm7_scaled<<15) \| ...`.

### Return

| Asm | Hex |
|---|---|
| `ret c30`                    | `0xC2C253C0` |
| `ret x30`                    | `0xD65F03C0` |

Cap-RET vs integer RET. Cap-RET uses LR (c30) as a tagged cap — the
hardware checks LR's tag, bounds, and exec perm before transferring
control.

## Concrete symptom from this session

The call_stub stub (StubRoutines::call_stub) starts execution at
PC=0x428b8138. Its first three emitted instructions, dumped at
runtime via diagnostic added to generate_call_stub:

```
+00: 0xa9be7bfd     // stp x29, x30, [sp, #-16]!    ← INTEGER stp (wrong)
+04: 0x910003fd     // mov x29, sp = add x29, sp, #0 ← INTEGER add (wrong)
+08: 0xd10683bf     // sub sp, x29, #0x1a0
```

These come from MacroAssembler's `enter()`:
```cpp
void enter() {
  stp(rfp, lr, Address(pre(sp, -2 * wordSize)));   // 0xa9be7bfd
  mov(rfp, sp);                                     // 0x910003fd
}
```

Both should be the cap variants:
```
+00: 0x62bf7bfd     // stp c29, c30, [csp, #-32]!
+04: 0x020003fd     // add c29, csp, #0           (or mov c29, csp = 0xc2c1d3fd)
```

Plus the `-2 * wordSize = -32` decrement only works with the cap-stp
variant where imm7 is scaled by 16; integer-stp's imm7 is scaled by 8,
so the current emit decrements sp by only 16 bytes (HALF the intended).

## Required patches (next session)

Each item below is a separate patch in the C-6/C-13 series:

1. **`stp(Reg, Reg, Address)` cap variant**. Pattern `0x42800000`
   (signed-imm9 offset) or `0x62B00000` (pre-index) family. Used by
   `enter()` and many save sequences.
2. **`ldp(Reg, Reg, Address)` cap variant**. Used by `leave()` and
   many restore sequences.
3. **`str(Reg, Address)` cap variant**. Used wherever a single oop /
   cap is stored.
4. **`ldr(Reg, Address)` cap variant**. Used wherever a single oop /
   cap is loaded.
5. **`Assembler::add(Reg, Reg, imm)` cap variant**, OR scoped use of
   the cap variant in the macroAssembler's `mov(Reg, sp/csp)` case.
   Pattern `0x02000000`.
6. **`ret()` cap variant**. Pattern `0xC2C253C0` for `ret c30`. Used
   at end of every stub.
7. Sweep callers of `enter()`, `leave()`, `ret()`, and all the above
   for completeness. Likely 50+ call sites.

Estimate (with the empirical encodings now in hand): each emit fix
is a 5-line change in macroAssembler_aarch64.{hpp,cpp}. The sweep is
mechanical. Plausibly one session per 2-3 instructions.

## Sanity check before any of those patches

Re-run the C6CS diagnostic (in javaCalls.cpp) to confirm the call_stub
function pointer is tagged correctly. Re-run the C6CS_BYTES diagnostic
(in stubGenerator_aarch64.cpp) AFTER the stp+mov fixes to confirm the
emitted bytes match the cap variants (0x62bxxxxx and 0xC2C1Dxxx).

## What the paper §3 gets out of this

This is the single most important "Porting Experience" datapoint of
the whole project: **HotSpot's AArch64 assembler is fundamentally
integer-only**. Every cap-typed operation needs a parallel cap
emitter. A complete CHERI port of HotSpot is in the ~30 separate
"cap variant" patches, each 5-50 LoC.

Compare to a clean-room "fresh GC for CHERI" approach: zero
assembler porting needed — we just need to emit machine code from
scratch where required. This validates the StoplessGC project's
strategic choice (a 3000-LoC fresh GC, vs. porting ZGC's 28000 LoC).
