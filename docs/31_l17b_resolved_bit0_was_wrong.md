# L17b RESOLVED — bit-0 hack was actively WRONG; cap-LDR for ConstMethod fix

**Date:** 2026-05-28 (continuation)
**Status:** C-6 layer 17 broken open; layers 18-19 newly visible.

## The actual bug at PC=0x428bc2c0

`ldr x3, [c12, #16]` — integer LDR via cap base. **In C64 mode this
loads only 64 address-bits into x3 (sets cap.tag=0).** Subsequent
`ldrh w2, [c3, #68]` then uses c3 (tag=0) as a cap base → PROT_CHERI_TAG.

What confused us yesterday:
* The instruction LOOKED valid (legal C64 encoding).
* gdb misdisassembled it as `ldp c9, c16, [c0, #-224]!` because PC
  was at 0x428bc2c1 (bit 0 set by our patch 0095), and gdb's
  disassembler started decoding from the misaligned PC → bytes
  shifted by 1.
* The bit-0 hack was supposed to set PSTATE.C64 via "Capability
  Value[0]" per ARM ARM §2.10. Observation says it doesn't — instead
  it just makes PC unaligned → SIGBUS alignment fault.

## What actually worked (L17b fix)

Two-part fix:

**Part A — javaCalls.cpp:** removed `| 1` from entry_point bit-0
manipulation. Pass entry_point as-is (4-byte aligned). BLR still
enters C64 mode (cpsr shows `C64` flag set after BLR), apparently
via cap metadata, NOT via Value[0]. Confirmed by gdb at fault.

**Part B — stubGenerator_aarch64.cpp:** removed the
`emit_int32(0x02000484)` (cap-ADD c4, c4, #1) right before
`__ blr(c_rarg4)`. Same reason — it was making PC unaligned, not
mode-switching anything.

**Part C — templateInterpreterGenerator_aarch64.cpp:1523:**
```cpp
#ifdef __CHERI_PURE_CAPABILITY__
  __ cap_ldr_imm(r3, rmethod, in_bytes(Method::const_offset()));
#else
  __ ldr(r3, constMethod);
#endif
```
Replaces integer LDR with cap-LDR so r3 (= c3) loads the full
ConstMethod cap with valid tag.

## Result — JVM ran 108 bytes (27 instructions) further

PC advanced from 0x428bc2c4 (SIGBUS) → 0x428bc330 (SIGPROT). The
new fault: `stp x20, x22, [csp, #-192]!`. csp is now tag-zero
(displayed by gdb as plain int, no `[rwRW...]` brackets), so the
pre-index store via csp fails.

Somewhere between PC=0x428bc2c4 (csp had tag=1) and PC=0x428bc330
(csp tag=0), an instruction clobbered csp's tag. Most likely:
- `__ add(sp, sp, ...)` or `__ mov(rscratch1, esp)` emitting
  integer ADD/MOV variant on the sp/csp aliases.
- This is C-6 layer 19, same pattern as L18.

## The general porting principle (now confirmed)

**Every register that holds a cap and is used as a pointer base
must be loaded/manipulated by cap-aware instructions, NOT by their
integer aliases.** HotSpot's `Register` type is the same for
integer and cap, so it's UP TO the call site to choose `cap_xxx`
helpers vs. plain `ldr/str/mov/add`.

The interpreter has ~256 bytecodes × ~10 instructions each = ~2500
instruction emits in the templateInterpreter (mostly). Probably
30-50% touch pointer/cap registers. So **estimate 600-1000 emits
need cap-aware variants**.

This is the actual cost of "porting HotSpot to CHERI" — and the
strategic argument for our StoplessGC approach (write a fresh
3000-LoC CHERI-native GC) vs porting any of the existing GCs
(28000-LoC mature with the same massive emit-rewriting needed).

## Next concrete step — L19

Find the emit that clobbers csp.tag between 0x428bc2c4 and
0x428bc330. Source range = generate_normal_entry lines 1532-1545
(generate_stack_overflow_check + rlocals setup). Patch with
cap-equivalent emit. Iterate.

## Layer count revised

The cascade went: L1..L17 = wrong assumptions about ARM ARM §2.10
Value[0] + register mode-switch. L17 RESOLVED by removing bit-0
hacks and switching the first cap-pointer load to cap-LDR.

L18 (first int-LDR-of-pointer): RESOLVED (this patch).
L19 (csp tag-loss): VISIBLE — next iteration.
L20..N: each subsequent emit-site that loads/manipulates a cap
  via integer instructions.

The cap-aware-emit cascade is now MECHANICAL. The strategic value
is FULLY CAPTURED by the count of these layers — every single one
is "HotSpot emits an integer instruction where a cap-aware one
is needed."

## Total session

**24 commits** (after this one).
**65 build iterations.**
**Layers reaching String.<clinit> interpreter prologue** (very
deep into HotSpot's method dispatch path, well past the call_stub
and BLR-to-entry layers we obsessed over yesterday).
