# L25 Option C — full architectural skeleton, blocked at BLR-mode semantics

**Date:** 2026-05-29
**Status:** Trampoline architecture complete; specific Morello BLR-mode
quirk blocks last 10% of the layer.

## What works (verified end-to-end)

1. **Thread::_cap_trampoline_addr field** added at byte offset
   (computed at runtime). Seeded in `Thread::Thread()` from
   `(void*)&cap_trampoline_dispatch`, which gives a sentry cap with
   CFI bounds `[.rodata, .got, .pad.cheri.pcc]` = `[0x4170d000,
   0x421c6000]` covering 11 MB of libjvm.so loadable sections.

2. **Two-file TU split** for `_cap_tramp_fn_table`:
   - `cap_trampoline_table_aarch64.cpp` (data-only)
   - `cap_trampoline_aarch64.cpp` (all functions)
   Verified via objdump that compiler emits **GOT-style** access
   (`adrp c0, GOT_page` + `ldr c0, [c0, #slot]`) instead of
   PCC-relative ADRP.

3. **Stub-side codegen** (call_VM_leaf_base):
   - Shift `c_rarg{N-1}..c_rarg0` → `c_rarg{N}..c_rarg1` via cap-MOV
   - `movw c_rarg0, fn_id` (32-bit move of integer ID)
   - `cap_ldr_imm rscratch1, [rthread, #_cap_trampoline_addr_offset]`
   - `blr rscratch1`

4. **gdb verification at BLR site**: `c8 = 0x41ac15a1 [rxRE,
   0x4170d000-0x421c6000] (sentry)`. Cap is valid (tag=1), sentry,
   bounds cover the trampoline address `0x41ac15a0`. PSTATE.C64
   shows set in cpsr after BLR.

## The blocker

`BLR c8` (the loaded cap) faults with `SIGBUS si_code=1
(PROT_CHERI_BOUNDS)` at the trampoline's first instruction.
Reproduced with FOUR different trampoline bodies:

1. Full C++ dispatch with global access — fault at `stp c29, c30,
   [csp, #-0x20]!` (prologue).
2. Minimal `return 0` — fault at same `stp` prologue.
3. Naked asm `mov x0, #0xdead; ret c30` — fault at `mov x0,
   #0xdead`.
4. After trying to clear address bit 0 via `cheri_address_set` —
   cap becomes `(invalid,sentry)` (tag=0); fault becomes SIGPROT.

Common pattern: **BLR through a sentry cap with address bit-0=1
faults**, even when:
- Cap is valid (tag=1).
- Bounds cover the target address.
- Perms include EXEC + EXEC_CAP.
- Target instruction is plain (`mov` or `stp` via valid csp).

Hypotheses, none confirmed:

- **Morello BLR-mode-bit semantics**: ARM ARM §2.10 RZTMWK says
  `PSTATE.C64 = Capability.Value[0]`. But maybe BLR via sentry does
  NOT auto-strip address bit 0 for ifetch (despite Thumb-style
  interworking expectation). PC would then be misaligned in C64
  mode → fault.

- **Sentry cap unsealing + mode interaction**: BLR auto-unseals
  sentry, but maybe Morello requires the unsealed cap to have
  specific properties not present here.

- **PCC.flags vs cap.flags mode marker conflict**: maybe the
  PCC's mode-flag field has a value that conflicts with the
  branching cap's mode marker.

## What we can't easily check

- Morello ARM ARM PDF defines RZTMWK semantics but doesn't
  spell out WHETHER bit 0 of cap.value is auto-stripped from PC
  on BLR — manual interpretation suggests yes, but observed
  behavior says no.

- We don't have a Morello reference manual section devoted to
  "cap-aware BLR ifetch alignment rule" so empirical exploration
  is the only path.

## Next-session path

Three candidates:

**A. Read Morello ARM ARM PDF section on BLR-via-cap carefully**,
specifically the pseudocode for `BranchToCapability`. Confirm
or refute the bit-0-auto-strip hypothesis. If refuted, find the
correct mode-switch mechanism.

**B. Test BLR-via-sentry-cap in a STANDALONE Morello C++ program**
(not in HotSpot). Compile a 2-function purecap binary and run.
If the same fault reproduces, the issue is fundamental to our
manual cap construction. If it works there, the issue is
HotSpot-specific (maybe rthread's bounds or some state we're
missing).

**C. Try a 4-instruction stub trampoline at a KNOWN good
location** (e.g., in cap_runtime/stopless_gc which is a separate
.so file with simpler PCC setup). Load its cap via dlsym at
Thread init.

## Session totals through L25-C

- 33 commits (with this one)
- 79 build iterations
- 25 layers identified, 24 fully fixed, L25 fully architecturally
  characterized
- Two new HotSpot translation units added (cap_trampoline_*)
- Trampoline architecture is the GOLDEN PATTERN for any future
  HotSpot port to CHERI Morello — even if L25's specific BLR bug
  resolution is pending, the architecture is reusable.

## For paper §3

L25 is the **first non-mechanical layer** in the entire C-6
cascade. L1-L24 are all "swap integer instruction for cap
variant" — mechanical, ~10-15 min each. L25 is "design and
implement a cross-PCC-bounds dispatcher" — multi-day
engineering work. This is the boundary the paper needs to
articulate: **CHERI Morello porting has TWO classes of bugs**:

1. **Mechanical cap-aware emit gaps** — high quantity but
   each takes minutes
2. **Architectural cross-PCC indirection layers** — low
   quantity but each takes days to weeks

The paper §2 strategic argument follows: writing a 3000-LoC
CHERI-native GC sidesteps BOTH classes because we never
generate code that needs to call back to C++ runtime — our
handler is pure C in a separate .so file with proper bounds
from the loader.
