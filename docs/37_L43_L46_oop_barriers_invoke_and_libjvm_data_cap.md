# L43–L46: oop barriers, field access, invoke setup — and the libjvm.so-data-cap pattern

**Date:** 2026-05-30 (Opus 4.8 session, continuation)
**Resolved this stretch:** L42 (cpcache stride), L43 (oop barriers),
L45 (invoke receiver). **Open:** L46 (invoke return table) — needs a
general mechanism, designed below.

## Resolved

**L42 cpcache stride = ×64, not ×32.** ConstantPoolCacheEntry is already
64 bytes on CHERI (intx = intptr_t = __intcap = 16 bytes; 4×16 = 64), a
power of two — no padding. The bug was the hardcoded ×32 (lsl #5) entry
stride; fixed to log2(size_in_bytes())=6 in get_cache_and_index_at_bcp +
get_cache_entry_pointer_at_bcp (the latter also needed cap-LDR for the
cpcache pointer + cap-ADD for header/index). Index 0 worked under ×32,
which is why 1600+ class-inits ran before the first index!=0 access.

**L43 GC barrier set oops as capabilities (high-leverage).**
BarrierSetAssembler::load_at/store_at used integer ldr/str for
uncompressed T_OBJECT/T_ARRAY — every oop load/store stripped the cap
tag. New cap_ldr/cap_str helpers dispatch on Address mode and, for the
register form, choose scaled (lsl#4, oop array) vs unscaled (uxtx,
byte-offset field) from the Address index shift (new
Address::index_shift()). Verified vs clang. Jumped ~2300 bytes of
execution past the getstatic/putstatic family.

**L45 invoke receiver via cap-add.** prepare_invoke computed the receiver
slot `add(rscratch1, esp, recv, uxtx, 3)`; integer add stripped esp's
tag. lsl + cap_add_reg.

## Open: L46 — invoke return entry table (the general pattern)

prepare_invoke loads the post-call interpreter continuation:
```cpp
const address table_addr = Interpreter::invoke_return_entry_table_for(code);
__ mov(rscratch1, table_addr);                              // (a) tag-0
__ ldr(lr, Address(rscratch1, rscratch2, Address::lsl(3))); // (b) ×8 (c) integer
```
Three faults in one:
* **(a) class-2**: `table_addr` is a libjvm.so .data global; integer mov
  yields a tag-0 base; DDC is null in purecap so PCC-relative adrp won't
  reach it either.
* **(b) class-3**: the table is `address[]`, 16-byte caps in purecap, but
  indexed lsl #3 (×8). Must be ×16.
* **(c)**: each entry is a code capability (the return continuation); it
  must be cap-LOADED so `lr` is a valid return cap, not an integer.

There are THREE such tables (invoke / invokeinterface / invokedynamic),
selected by `code` at codegen.

### This is the THIRD class-2 libjvm.so-global-table occurrence

L32 (SkipIfEqual flag → resolved at codegen time), L34 (dispatch table →
Thread::_cap_dispatch), now L46 (invoke return tables). It will keep
recurring (safepoint dispatch table, etc.). Per-table Thread fields do
not scale.

### The general fix to build (next focused session)

Seed ONE wide capability covering libjvm.so's data once, reachable via
rthread, plus a codegen helper:
```
lea_libjvm_global(reg, addr):
   cap_ldr_imm(reg, rthread, _cap_libjvm_data_offset)   // wide .data cap
   <materialize addr into tmp>
   scvalue reg, reg, tmp                                 // reg.addr = addr
```
Then any `mov(reg, libjvm_global); use [reg]` becomes
`lea_libjvm_global(reg, global); use [reg]` (with cap-LDR + correct
stride at the use site).

**The hard part — obtaining a wide .data cap in C++.** Observations:
* `&global` caps are TIGHT (the dispatch-table cap was exactly 64 KB),
  so subobject/variable bounds are enabled — a single global won't span
  the segment.
* A function-pointer cap spans [.rodata,.got] (≈11 MB, the CFI region)
  but NOT .data/.bss (the invoke table at 0x42283330 is past its top
  0x421c6000).
* DDC is null (`mrs DDC` → untagged) in purecap.

Candidate sources for a segment-wide .data cap:
1. Linker-defined span symbols (e.g. `__data_start`/`_edata`) — take a
   cap to one and `cheri_bounds_set` to `[__data_start, _edata)` (this
   NARROWS a wider parent, so the parent must already be wide — chicken/
   egg unless the symbol cap is segment-wide).
2. The RTLD per-DSO data cap (CheriBSD `Obj_Entry`/captable base) — may
   be reachable via `dlinfo`/`_dl_*` or a libc hook.
3. Build with `-cheri-bounds=conservative` (subobject bounds off) for the
   TUs that seed the cap, so `&global` spans the segment.
4. Compute the span from two distant globals' addresses and rederive
   from whichever cap has the widest bounds.

Option 3 (a per-file or per-symbol bounds relaxation for the seeding
helper) is the most promising and least invasive; needs verifying the
Morello clang flag and that the resulting cap covers .data+.bss.

Once `_cap_libjvm_data` exists, L46 and every future class-2 table is a
two-line change at each site.

## Session tally (L25–L45)

45 commits, 109 build iterations. The JVM now: executes bytecode
templates, dispatches all bytecodes, resolves + reads/writes
constant-pool-cache field entries, loads/stores oops through the GC
barrier set as capabilities, and sets up invoke (receiver loaded). The
remaining interpreter work is dominated by (1) the libjvm.so-data-cap
mechanism for global tables, then (2) the tail of mechanical
cap-arithmetic in the invoke/return path.
