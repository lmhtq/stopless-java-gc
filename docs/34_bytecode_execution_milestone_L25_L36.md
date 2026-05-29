# MILESTONE: interpreter executes bytecode + 1600+ class-init events (L25–L36)

**Date:** 2026-05-29 (Opus 4.8 session continuation)
**Arc:** from "JVM crashes ~5 instructions into the interpreter prologue"
to "interpreter dispatches & executes bytecode templates; >1600
class-initialization field events; dozens of bootstrap classes loaded."

## The breakthrough chain

**L25 — cap-BLR (the keystone).** HotSpot's `Assembler::blr` emits the
INTEGER `blr Xn`, which only moves PC *within the current PCC bounds*. A
codecache stub calling a libjvm.so address moved PC but kept codecache
PCC bounds → PROT_CHERI_BOUNDS on the first ifetch. Morello capability
`BLR Cn` (0xC2C23000 | Rn<<5, verified vs clang) installs Cn as the NEW
PCC. This unblocked the trampoline and the entire codecache↔libjvm.so
boundary. (patches 0099–0100.)

**L26–L31 — interpreter prologue cap-aware.** (patch 0101.)
- L26 trampoline arg-shift miscount → fixed 5-reg shift.
- L27/L28 rmethod (r12, caller-saved) clobbered across call_VM → reload
  via cap-LDR; all interp getters → cap-LDR/STR.
- L29 generate_fixed_frame integer stp strips tags + 8B stride misplaces
  cap-sized (16B) frame slots → cap_stp_sp/cap_stp_sp_pre.
- L30 MethodCounters* via cap-LDR.
- L31 bang_stack integer sub strips sp tag → cap-SUB.

**L32–L35 — dispatch + execution.** (patch 0102.)
- L32 SkipIfEqual global-flag check (DTraceMethodProbes …) via adrp can't
  reach libjvm.so .bss; DDC is null in purecap. Flags are init-time
  constant → resolve at CODEGEN time, emit a static branch.
- L33 rbcp = ConstMethod + codes_offset via integer add → cap-ADD.
- L34 dispatch table (libjvm.so .bss global) unreachable via adrp →
  Thread::_cap_dispatch seeded in C++, loaded via rthread.
- L35 esp (expr-stack ptr) = integer sub of sp → cap-SUB. This unblocked
  EVERY bytecode template (operand push/pop via [esp]).

**L36 (CURRENT) — trampoline-return cap corruption.** After L35,
templates execute and call runtime helpers via the trampoline. On
RETURN from the trampoline (`cap_blr c8` → fn_id 19 → ret), PCC is
corrupted: its address becomes the *metadata word* of the codecache cap
(0xfc5fc000116e516c) instead of the return address (0x428bc878). The
clr/c30 inspected post-fault is VALID (addr 0x428bc878, codecache
bounds), so the corruption happens transiently during the trampoline's
`ret c30`. Hypotheses:
  1. The trampoline's saved c30 (spilled in its prologue, restored in
     epilogue) is mismatched somewhere, swapping address/metadata.
  2. A sentry-unseal interaction in `ret c30` after our `BLR Cn`-created
     return sentry.
  3. fn_id 19's runtime function perturbs the stack slot holding the
     saved return cap.
Needs careful gdb single-stepping through the trampoline epilogue
(slow on the WITNESS-debug kernel) next session.

## How far we got (iter 94)

* 1676 `[C6ISF]` static-field-init events — classes initialized include
  String, StringBuilder, BufferedReader, many java.lang/java.io/java.util
  bootstrap classes (T_LONG, $assertionsDisabled, serialVersionUID …).
* Bytecode templates execute (observed the `ixor` template popping two
  operands off the expression stack).
* call_VM / call_VM_leaf runtime callbacks reach real libjvm.so C++ code
  via the trampoline and (mostly) return.

## Reusable CHERI-Morello porting primitives built this arc

In macroAssembler_aarch64.hpp:
- `cap_blr` / `cap_br` — Morello capability branch (installs new PCC).
- `cap_stp_sp` / `cap_stp_sp_pre` — cap-STP to [sp,#imm] (sp encodes #31).
- `cap_lea_global` — DDC+SCVALUE global addressing (kept for reference;
  DDC is null in purecap so currently unused).
- existing: cap_str_imm/cap_ldr_imm/cap_stp_imm/cap_ldp_imm/cap_sub_imm.

Thread fields: `_cap_trampoline_addr` (L25), `_cap_dispatch` (L34).

Two libjvm.so TUs: cap_trampoline_aarch64.cpp (dispatch) +
cap_trampoline_table_aarch64.cpp (GOT-forced table global).

## The two-class porting taxonomy (paper §3)

1. **Mechanical cap-aware-emit gaps** (L18–L23, L27–L35): swap an
   integer ldr/str/stp/add/sub for its cap variant. Minutes each.
2. **Architectural cross-PCC indirection** (L25 calls, L34 data): the
   codecache PCC cannot reach libjvm.so; needs cap-BLR + a
   thread-seeded cap (calls → trampoline, data → _cap_dispatch). DDC is
   null in purecap, so PCC-relative adrp to libjvm.so globals is a dead
   end; everything must come via a register-reachable seeded cap.

A full HotSpot CHERI-Morello port would have ~100 class-1 fixes and
~5–10 class-2 architectural pieces. StoplessGC's CHERI-native design
sidesteps the whole family: the GC handler is pure cap_runtime C in a
separate .so with RTLD-provided bounds — no generated code calling back
into the C++ runtime.

## Diagnostic note (L36 cleanup)

The diagnostic SIGILL/SIGSEGV handlers in cap_runtime/handler.c read raw
instruction bytes around the faulting PC and themselves took nested
capability faults, obscuring the real fault. They are no longer
installed (only SIGPROT). The SIGPROT handler has a DIAG abort-after-3
to break the retry-without-advance loop on bounds faults it cannot
forward.

## Next session

1. Single-step the trampoline `ret c30` to catch where the return cap's
   address/metadata get swapped (L36).
2. Likely fix: ensure the trampoline preserves the BLR-created sentry
   intact, or adjust how cap_blr seeds c30, or audit fn_id 19's stack
   usage.
3. Then continue popping template/runtime cap faults until
   `java -version` prints and exits 0.

Session: 36 commits, 94 build iterations, layers L25–L36 (L25–L35
resolved, L36 precisely located).
