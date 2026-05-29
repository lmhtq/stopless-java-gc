# L42 — STRUCTURAL: ConstantPoolCacheEntry is 48 bytes on CHERI, breaks ×32 stride

**Date:** 2026-05-29
**Class:** 3 (layout/size mismatch) — NOT a mechanical cap-LDR sweep.
**Status:** characterized + partially fixed (the cap-LDR field loads);
the stride/size fix is deferred to a focused session.

## The finding

`ConstantPoolCacheEntry` (cpCache.hpp):
```c
volatile intx     _indices;   // @0,  8 bytes
Metadata* volatile _f1;       // CAP, 16-byte aligned → @16, 16 bytes
volatile intx     _f2;        // @32, 8 bytes
volatile intx     _flags;     // @40, 8 bytes
```
On CHERI purecap `Metadata*` is a 16-byte capability with 16-byte
alignment, so 8 bytes of padding follow `_indices`. Result:
**sizeof(ConstantPoolCacheEntry) == 48**, a NON-power-of-2, vs 32 on
non-CHERI (4 × 8-byte words).

The interpreter bakes in the 32-byte (power-of-2) assumption:
* `InterpreterMacroAssembler::get_cache_and_index_at_bcp` line 225:
  `add(cache, rcpool, index, Assembler::LSL, 5)` — ×32.
* templateTable codegen similarly indexes with `lsl #5`.
* asserts `sizeof == 4*wordSize` and `exact_log2(size_in_bytes) ==
  2+LogBytesPerWord (==5)` exist but are compiled out in this optimized
  build, so the mismatch is silent.

## Why it manifested only now (and only sometimes)

For cpcache **index 0** the stride is irrelevant (idx×anything = 0), so
the very first getstatic/getfield worked and we got deep into bootstrap.
The fault appears when an entry with index != 0 is accessed: `idx×32`
lands in the middle of the previous 48-byte entry, so `_f1` reads back
NULL, and the subsequent (now correctly cap-LDR, patch 0107)
java_mirror load dereferences null → SIGPROT at the +mirror_offset load.

gdb confirmation (iter 104):
```
0x428c8918: ldr c2, [c2, #96]     ; f1 = *(cpcache + idx*32 + cp_base + 16) → NULL (wrong entry)
0x428c891c: ldr c2, [c2, #208]    ; *(NULL + java_mirror_offset) → FAULT
c2 = 0x0
```

## The fix (next focused session)

Two viable approaches:

**A. Pad ConstantPoolCacheEntry to a power-of-2 (64 bytes) on CHERI.**
- Add a `_pad` field (or arrange so sizeof rounds to 64) under
  `#ifdef __CHERI_PURE_CAPABILITY__`.
- Update every hardcoded `LSL 5` (×32) entry-stride to `LSL 6` (×64)
  on CHERI.
- size_in_bytes()/size() already use sizeof, so allocation follows.
- Re-verify f1_offset/f2_offset/flags_offset (still 16/32/40 within the
  64-byte entry; the +cp_base offsets in templates stay correct).
- Pro: keeps the cheap shift-based indexing. Con: must find ALL the
  hardcoded ×32 sites (get_cache_and_index_at_bcp,
  get_cache_entry_pointer_at_bcp, and any templateTable inline ones).

**B. Replace the shift with a computed ×sizeof multiply.**
- `mov tmp, size_in_bytes(); madd cache, index, tmp, rcpool` (or mul +
  cap_add_reg). Works for any size.
- Pro: no struct padding, robust to size. Con: more instructions per
  cpcache access; still must find all sites.

Approach A is recommended (keeps codegen shape, one-time struct change).

### Audit checklist for the stride sites
```
grep -n "LSL, 5\|lsl.*5\|<< 5\|ConstantPoolCacheEntry::size\|in_words(ConstantPoolCacheEntry" \
     src/hotspot/cpu/aarch64/interp_masm_aarch64.cpp \
     src/hotspot/cpu/aarch64/templateTable_aarch64.cpp
```
Also check get_cache_entry_pointer_at_bcp (interp_masm:247) which has the
matching `assert(exact_log2(...) == 2 + LogBytesPerWord)` and its own
`add ..., LSL ...`.

## Already landed (patch 0107, correct regardless of the stride fix)

- load_field_cp_cache_entry: f1 (holder Klass*) + java_mirror → cap-LDR.
- load_invoke_cp_cache_entry: resolved Method* → cap-LDR.

These are necessary (the fields ARE capabilities); they're just not
sufficient until the entry stride is corrected so the right entry is
addressed.

## Broader implication (paper §3, class-3 issues)

Any HotSpot metadata struct that mixes pointers (16-byte caps) with
power-of-2-stride array indexing breaks on CHERI when its size stops
being a power of 2. ConstantPoolCacheEntry is the first; others to audit:
- ConstantPoolCache header (base_offset).
- Klass / InstanceKlass vtable/itable strides.
- Method / ConstMethod.
- The interpreter frame (already handled, L29, with explicit 16-byte
  cap-STP stride).

This is the subtlest CHERI porting class: the code is "correct" for
small/zero indices and corrupts silently at scale. It strengthens the
StoplessGC strategic argument — a CHERI-native GC defines its own
cap-aware data structures from the start rather than inheriting 25 years
of power-of-2-stride assumptions baked into HotSpot's metadata layout +
interpreter codegen.

## Session close (L25–L42)

41 commits, 104 build iterations. The JVM went from crashing ~5
instructions into the interpreter prologue to executing bytecode
templates, initializing 1600+ classes, resolving constant-pool entries,
calling runtime across the codecache↔libjvm.so boundary, and walking the
Java stack from C++ — all cap-correct. L42 is the first class-3
structural layout issue and the clean handoff point.
