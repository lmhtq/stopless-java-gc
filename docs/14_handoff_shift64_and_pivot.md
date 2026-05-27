# Handoff: shift=64 crash + decision to pivot to ZGC barrier prototype

**Date:** 2026-05-27 (late afternoon)
**Status:** Investigation paused after multiple instrumentation attempts;
            pivoting to parallel ZGC barrier prototype work.

## What we know about the shift=64 crash

After patches 0066-0070, JVM init reaches:
```
[BEGIN] StubRoutines::forward_exception   ← successfully generated
                                          ← crashes during generate_call_stub
                                            (the next stub after forward_exception)
Internal Error (assembler_aarch64.hpp guarantee):
  val=0x40 msb=15 lsb=10 nbits=6   "Field too big for insn"
```

`val=64` in a 6-bit field at [15:10]. Bits [15:10] is the `imms` field
in bitfield-move-family instructions (sbfm/bfm/ubfm). Some shift or
field width has been computed wrong.

## Instrumentation attempts (all reverted)

1. **`__builtin_return_address(0..3)`** — RA[0] works (gives immediate
   caller of f()), but N≥1 unreliable on CHERI (returns garbage like
   ConcurrentHashTable when actual chain is Assembler/StubGenerator).

2. **Cap-aware FP-chain walk via `mov %0, c29`** — got 12 frames
   reliably for op_shifted_reg (shift=64 path). But for f() at
   bfm/sbfm/ubfm, the walker returns LRs that resolve to seemingly
   unrelated functions (ConcurrentHashTable, StringTable). Cause:
   addr2line maps inlined helpers to their containing C++ symbol, not
   the caller. The inline depth makes interpretation hard.

3. **`PrintStubCode`** — narrowed crash to `generate_call_stub`
   (the stub after forward_exception). But this 200-line function has
   many candidate instructions; manual audit didn't find shift=64.

## What likely causes shift=64 / imms=64

The pattern: `wordSize`-based shift or field-width calculation that
gives 8 under standard LP64 but **16 under CHERI** (since wordSize =
sizeof(intptr_t) = 16). Examples we've already fixed (0059, 0069):
- `4 * wordSize` for SIMD stride
- `wordSize * 2` for `ldpq` pair offsets

The unsolved one is probably in:
- `__ enter()` macro
- `__ stp/ldp` of cap/int register pairs (line 244-251 of
  generate_call_stub)
- Some bfi/bfxil/sbfx/ubfx call with width that becomes 64+

A specific hypothesis worth trying: `BitsPerWord = 1 << LogBitsPerWord
= 1 << (LogBitsPerByte + LogBytesPerWord) = 1 << (3 + 3) = 64`. If
any code does `bfi(rd, src, lsb, BitsPerWord)`, it gives
`width=64, imms = lsb + 63`. If lsb > 0, imms > 63 → crash.

Even with `LogBytesPerWord` unchanged from 3 (as we left it),
`BitsPerWord = 64` is a value that, combined with non-zero lsb,
trips the bfi imms check.

## Why we're pivoting

After ~3 hours on this single bug today:
- 5+ instrumentation iterations, ~60-90s each
- Speed is the bottleneck, not understanding
- CHERI debug infrastructure (no debugger, untrustworthy backtrace)
  makes each round slow

The shift=64 fix is in the **Phase 1 long tail** — it doesn't directly
unlock the **research contribution** (ZGC cap-load barrier).

## Pivot plan: parallel ZGC barrier prototype (no-JVM-required)

Develop a **standalone CHERI cap-load barrier mechanism** as a C++
program that:
1. Sets up a small "heap" region with mmap
2. Stores oop-like pointers (raw integers) representing from-space refs
3. Implements `cap_load_barrier(int* heap_ptr)`:
   - Load cap from heap_ptr
   - If tag bit zero → SIGPROT handler → rematerialize via
     forwarding table lookup
   - Return updated cap, optionally self-heal store
4. Measures: instruction count vs equivalent ZGC software AND-CBNZ

This validates the mechanism without needing JVM to boot. **The
paper's §4-5 (Barrier Design + Implementation) can be written from
this prototype**, with §3 (Porting Experience) using the current
patch series.

Estimated work: 1-2 weeks for prototype + measurement harness.

## Resume points for shift=64 (when picked up later)

1. Read `generate_call_stub` from beginning more carefully, looking
   for any `bfi`/`bfxil`/`sbfx`/`ubfx`/`lsl`/`lsr`/`asr` with non-
   constant arguments that could compute to 64.

2. Try alt-instrumentation: register `signal(SIGABRT, ...)` and inside
   the handler walk caller-via-getcontext-ucontext. That gives the
   real PC + register state at trap.

3. Or: enable `-XX:+CompileTheWorld` style stress, run a SHORTER
   subset of stub generation to isolate which stub hits the issue.

4. Or: differential debugging — compile the same `generate_call_stub`
   for standard aarch64 (non-CHERI cross), disasm, compare which
   instructions differ when CHERI is on.
