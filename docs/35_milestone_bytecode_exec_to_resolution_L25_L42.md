# MILESTONE: bytecode exec → constant-pool resolution → C++ frame walking (L25–L42)

**Date:** 2026-05-29 (Opus 4.8 session)
**Arc this session:** from "JVM crashes ~5 instructions into the
interpreter prologue" to "interpreter executes bytecode templates,
initializes 1600+ classes, resolves constant-pool entries, calls runtime
helpers across the codecache↔libjvm.so boundary, and walks the Java
stack from C++ — all cap-correct." 17 layers (L25–L41) resolved; L42 is
the start of a well-characterized mechanical sweep.

## Layer ledger (this session)

| L | Bug | Fix | Patch |
|---|-----|-----|-------|
| 25 | integer `blr Xn` keeps codecache PCC → can't reach libjvm.so | Morello `blr Cn` (cap-BLR) installs new PCC + trampoline | 0099/0100 |
| 26 | trampoline arg-shift miscount | fixed 5-reg shift | 0101 |
| 27/28 | rmethod (r12) clobbered across call_VM | reload via cap-LDR; all interp getters cap-LDR | 0101 |
| 29 | frame stp strips tags + 8B stride vs 16B slots | cap_stp_sp / cap_stp_sp_pre | 0101 |
| 30 | MethodCounters* via integer ldr | cap-LDR | 0101 |
| 31 | bang_stack integer sub strips sp | cap-SUB | 0101 |
| 32 | SkipIfEqual adrp can't reach libjvm.so .bss; DDC null | resolve flag at CODEGEN time | 0102 |
| 33 | rbcp = ConstMethod+codes via integer add | cap-ADD | 0102 |
| 34 | dispatch table (libjvm.so .bss) via adrp | Thread::_cap_dispatch seeded in C++ | 0102 |
| 35 | esp = sp - N via integer sub | cap-SUB (unblocked all templates) | 0102 |
| 36/37 | **dispatch table entries are 16B caps, indexed ×8** → odd bytecodes read metadata half | ×16 via lslw + uxtw(0) | 0103/0104 |
| 38 | SafeFetch probe faults SIGPROT, not handled | cap_runtime SIGPROT handler redirects to continuation | 0104 |
| 39 | cpcache index `add rcpool,idx,LSL5` strips tag | cap_add_reg | 0105 |
| 40 | `Address::lea` integer add/sub strips base cap | cap-aware lea (cap_add_imm/sub_imm/add_reg) — high leverage | 0105 |
| 41 | set_last_Java_frame stores anchor via integer str | cap-STR all 3 anchor slots — high leverage | 0106 |
| **42** | template loads resolved cpcache _f1 (Klass* cap) via integer ldr | **mechanical sweep of templateTable cap-field loads — NEXT** | — |

## The keystone: L25 cap-BLR

HotSpot's `Assembler::blr` emits the integer `blr Xn`, which on Morello
only updates PC *within the current PCC bounds*. A codecache stub
(PCC = [code_cache]) branching to a libjvm.so address moves PC but keeps
codecache bounds → PROT_CHERI_BOUNDS on the first ifetch. The Morello
capability `BLR Cn` (0xC2C23000 | Rn<<5) installs Cn as the new PCC. This
single ISA fact unblocked the entire codecache↔runtime boundary and is
the reusable foundation for any HotSpot CHERI-Morello port.

## The "runs far then crashes randomly" mystery: L36/L37

The dispatch table is `address _table[states][256]`, and `address` is a
16-byte capability in purecap, but the generated dispatch indexed it with
uxtw(3) (×8). EVEN bytecodes read the entry's address half (the real
handler PC → worked); ODD bytecodes read the previous entry's metadata
half (0xfc5fc000116e516c) and branched into garbage. This is why the JVM
ran 1600+ class-init events (the even-indexed bytecodes on the hot path)
before crashing on the first odd-indexed dispatch. Fixed with ×16
indexing (lslw #4 + uxtw(0), since the addressing mode forbids a ×16
scaled load).

## The two-class porting taxonomy (refined, for paper §3)

1. **Mechanical cap-aware-emit gaps** — swap an integer
   ldr/str/stp/add/sub for its cap variant. Minutes each; high count.
   L18–L23, L27–L31, L33, L35, L39, L40, L41, and the upcoming L42 sweep.
   Several were HIGH-LEVERAGE single fixes that unblocked many sites at
   once: esp (L35), Address::lea (L40), set_last_Java_frame (L41).

2. **Architectural cross-PCC indirection** — the codecache PCC cannot
   reach libjvm.so; DDC is null in purecap, so PCC-relative adrp to
   libjvm.so is a dead end. Everything must come via a register-reachable
   seeded capability: calls → trampoline + cap-BLR (L25), data globals →
   Thread::_cap_dispatch (L34) / codegen-time resolution (L32).

3. **Layout/size mismatches** — `address`/pointer is 16 bytes in purecap,
   so any table or frame indexed with the non-CHERI 8-byte stride is
   wrong: dispatch table (L36), interpreter frame slots (L29). These are
   the subtlest — they "work" for a while then corrupt.

A full HotSpot CHERI-Morello port would have ~100 class-1, ~5–10 class-2,
and ~10–20 class-3 issues. StoplessGC's CHERI-native design sidesteps the
entire family: the GC handler is pure cap_runtime C in a separate .so
with RTLD-provided bounds; it never generates code that calls back into
the C++ runtime, never builds an interpreter dispatch table, never walks
HotSpot frames from generated code.

## Reusable primitives built (macroAssembler_aarch64.hpp)

cap_str_imm, cap_ldr_imm, cap_stp_imm, cap_ldp_imm, cap_sub_imm,
cap_add_imm, cap_stp_sp, cap_stp_sp_pre, cap_blr, cap_br, cap_add_reg,
cap_lea_global (DDC — unused, kept for reference).
Thread fields: _cap_trampoline_addr, _cap_dispatch.
cap_runtime: SIGPROT handler with SafeFetch redirect + forward-table
self-heal; trampoline dispatch (cap_trampoline_aarch64.cpp + table TU).

## Current state (iter 103)

`java -version` under -XX:+UseStoplessGC reaches deep bytecode execution:
templates dispatch correctly for all bytecodes, getstatic triggers
constant-pool resolution, the resolve runtime call returns cleanly
through C++ frame walking, and execution resumes in the getstatic
template. The fault is now L42 — loading the resolved cpcache `_f1`
(Klass* cap) with integer ldr.

## Next session — the L42 sweep

templateTable_aarch64.cpp loads ConstantPoolCacheEntry fields (_f1 =
Klass*/Method* cap, _f2 = offset/Method*, flags = int) in getstatic/
getfield/putfield/invoke*/new/etc. Each metadata-pointer load needs
cap-LDR. Approach: sweep templateTable converting `ldr(reg,
Address(cache, f1/f2 offset))` for the pointer fields to cap-LDR in one
batch, then iterate on the remainder. Expect this to cross <clinit>
completion and approach `java -version` printing.

Estimate: 10–25 more layers (mostly mechanical) to `java -version` exit 0,
several of them high-leverage shared helpers (field load, invoke dispatch,
oop load/store barriers).

Session totals: 40 commits, 103 build iterations, L25–L41 resolved + L42
characterized.
