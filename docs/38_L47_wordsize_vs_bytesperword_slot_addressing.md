# L47/L47b: wordSize ≠ BytesPerWord — 16-byte interpreter slots vs 8-byte values

**Date:** 2026-05-30 (Opus 4.8 session)
**Root cause found, partial fix landed, systematic remainder specified.**

## The root cause (L47)

On CHERI purecap the two "word" notions HotSpot conflates on every other
platform are DIFFERENT:
* `wordSize = sizeof(char*) = 16` (a machine word / pointer = a capability)
* `BytesPerWord = 1 << LogBytesPerWord = 8` (the *Java* word)

Equal on non-CHERI (both 8); they diverge on CHERI. Consequences in the
interpreter:
```
stackElementSize    = stackElementWords(1) * wordSize     = 16   ✓ (slots hold caps)
logStackElementSize = LogBytesPerWord                     = 3    ✗ (should be log2(16)=4)
```
The interpreter's expression stack AND locals use 16-byte slots (an oop is
a 16-byte cap and must fit in one slot — confirmed by the templates'
`ldr w0, [esp], #16` pop and `add esp, esp, stackElementSize(=16)`). But
`logStackElementSize` was the JAVA-word log (3), so every *variable-index*
slot access that shifts by it strides ×8 over 16-byte slots → wrong slot.

Patch 0112 set `logStackElementSize = 4` under `#ifdef
__CHERI_PURE_CAPABILITY__` and fixed prepare_invoke's receiver shift. The
constant-index fast forms (iload_0, at_tos, local_offset_in_bytes(n) =
n*16, expr_offset_in_bytes) were already correct because they use
stackElementSize directly; only the variable-index forms were ×8.

## The tension that makes L47b non-trivial

A 16-byte slot often holds an 8-byte value (int/float/long/double). To
load it by variable index you'd want `ldr Xt, [base, index, lsl #4]`
(index × 16-byte slot). But the AArch64/Morello scaled register-offset
addressing mode requires the shift to be 0 OR log2(access size). For an
8-byte `ldr` that's 0 or 3 — **a shift of 4 is illegal**
(assembler_aarch64.hpp:542 `guarantee(_ext.shift() <= 0 || _ext.shift()
== (int)size)` → "bad shift"; observed as a fatal Internal Error).

So you cannot scale by 16 in a single sub-16-byte load. Each
variable-index access to a 16-byte slot must instead:
* `lea`/cap-add the slot address first (no shift constraint — the
  `laddress(r,scratch,_masm)` form already does this and works), then
* load the value with offset 0 / `local_offset_in_bytes`.

This is exactly why `iaddress(Register r)` (which returns a bare
`Address(rlocals, r, lsl#?)` consumed directly by `ldr`) can't simply be
bumped to ×16, while `laddress` (lea-based) can.

## Current state (kept consistent and RUNNING)

To avoid a half-converted, self-inconsistent locals layout, the
variable-index locals are all left at ×8 for now (iaddress, laddress, and
the lload/dload/lstore/dstore `sub(rlocals, idx, LogBytesPerWord)` forms),
matching each other and the original behaviour. `logStackElementSize`
stays at 4 (the correct value, used by the expression-stack /
prepare_invoke side); the locals ×8 sites are marked `C-6 L47b`.

The JVM therefore still faults at the invoke receiver: a variable-index
`aload` reads the wrong (×8) local and pushes a stray value, so the
receiver computed from the expression stack is wrong (a codecache address
instead of the heap oop, which sits one 16-byte slot away).

## L47b — the systematic fix (next focused session)

Convert ALL variable-index slot accesses (locals + any expression-stack
register-index forms) to compute the slot address first:

1. `iaddress(Register r)` / `aaddress` / `faddress`: change the call sites
   from `__ ldr(dst, iaddress(r))` to compute the slot address via lea
   (`__ lea(scratch, Address(rlocals, r, lsl logStackElementSize)); __
   ldr(dst, Address(scratch, 0))`), or give iaddress a `_masm`+scratch
   signature like laddress. ~12 direct ldr/str call sites (templateTable
   lines 632/640/643/650/682/694/728/849/1014/1051/1552/1554) plus the 4
   lea sites which already work once iaddress uses ×16.
2. lload/dload/lstore/dstore `sub(r1, rlocals, r1, uxtw, LogBytesPerWord)`
   → `logStackElementSize`.
3. Audit interp_masm and any other `lsl(logStackElementSize)` used as a
   *scaled-load* shift on an 8-byte access — those need the same lea
   treatment (interp_masm:1925 is one; it survived only because its path
   isn't on the basic-interpreter codegen path, or its access is cap-wide).
4. For OOP locals specifically (aaddress), the load IS a 16-byte cap load,
   so `ldr c, [base, idx, lsl#4]` (cap_ldr) is legal and can stay scaled.

Doing all of (1)-(3) atomically keeps locals self-consistent and finally
makes the variable-index `aload` read the right oop → the invoke receiver
becomes valid → the first real method dispatch proceeds.

## Why this is a distinct (and important) CHERI porting class

This is the sharpest example of class-3 (layout/size) issues: the slot
size doubled (8→16) because pointers are caps, but the *values* in slots
didn't, and the load addressing modes are sized to the value. Every
interpreter that stores mixed value/pointer data in uniform machine-word
slots hits this on CHERI. It is invisible on constant-index fast paths
and only bites on variable-index access — exactly the kind of latent,
scale-dependent corruption that makes a 28kLoC interpreter port to CHERI
treacherous, and that a fresh CHERI-native GC (StoplessGC) never inherits.

## Session tally (L25–L47)

48 commits, 112+ build iterations. JVM executes bytecode, dispatches all
bytecodes, resolves+accesses cpcache field entries, loads/stores oops as
caps (barrier set), reaches invoke setup with a working invoke-return
table via the new general libjvm.so-data cap mechanism (L46). The last
interpreter blocker before method dispatch is the L47b variable-index
slot-addressing conversion specified above.
