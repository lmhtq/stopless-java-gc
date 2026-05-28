# L25: CodeCache PCC bounds vs libjvm.so target — architectural impasse

**Date:** 2026-05-28 (continuation of docs/31)
**Status:** L25 half-solved; full resolution requires HotSpot codegen
redesign, not a single-emit fix.

## What we proved

The pattern `mov rscratch1, entry_point; blr rscratch1` in
`call_VM_leaf_base` constructs rscratch1 as an INTEGER (cap tag=0)
which BLR rejects. Replacing with `adrp + cap-ADD + blr` (patch 0098)
produces rscratch1 with a VALID cap (tag=1).

**However:** the resulting cap inherits PCC's bounds. PCC at the
interpreter stub PC = [0x428b4000, 0x448b4000) = 32 MB code cache.
The runtime function entry_point lives in libjvm.so .text at
~0x41c8e000, which is **OUTSIDE** the code-cache bounds.

CHERI Morello caps allow address-out-of-bounds (cap remains tag=1)
but any access via that cap faults with PROT_CHERI_BOUNDS. So the
BLR sees a valid cap with out-of-bounds address → SIGBUS.

```
c8 = 0x41c8e74d [rwxRWE, 0x428b4000-0x448b4000]
              ^^^^^^^^^^                  ^^^^^^^^^^
              address ≪ base — out of bounds, BLR faults
```

## Why this is hard to fix

CHERI fundamentally **cannot widen capabilities** — only narrow.
There is no instruction "make this cap span both code cache and
libjvm.so". Each cap derives from a parent via NARROWING.

Options for getting a wide-enough cap:

1. **GOT-style indirect call.** Linker creates a data section with
   pre-populated function caps (each with right bounds). Codegen emits
   `ldr ct, [data_cap, GOT_slot]; blr ct`. HotSpot has no GOT
   infrastructure — would need adding one.

2. **Runtime call table.** At JVM init time, walk a list of runtime
   functions and store wide-bounds caps to each in a per-thread or
   global table. Codegen looks them up at runtime. Moderate effort.

3. **Pass entry_point cap as parameter.** Each call_VM site already
   passes entry_point as a function argument from C++. The C++ side
   knows the cap (compiler links it correctly). We could store this
   cap in a register before the stub dispatch, then BLR via the
   register. Lightest infrastructure but requires plumbing through
   all call_VM_base / call_VM_leaf paths.

## Why deferred

Each option above is a HotSpot architectural change (~3-7 source
files, hundreds of lines), not a single-emit fix. The session has
already covered 24 layers across 7+ source files with 100+ targeted
edits. L25 is the natural session boundary.

## What the paper §3 ("porting experience") gains

L25 is the FIRST layer that ISN'T fixable by "emit cap variant of
this integer instruction." It's the first layer requiring a
HotSpot architectural change. This is the natural inflection point
that demonstrates *why* a 28000-LoC mature-GC port to CHERI Morello
is fundamentally harder than the L1-L24 fix pattern suggests:

* L1-L24 are MECHANICAL (find emit, swap variant).
* L25+ are ARCHITECTURAL (codegen needs GOT / call-table / arg
  passing infrastructure).
* A full HotSpot port would have ~100 L1-L24-style issues + ~5-10
  L25-style issues. Each L25-style issue is its own multi-week
  engineering project.

## Session totals through L25 attempt

- 30 commits (including this docs/32 + patch 0098)
- 71 build iterations
- 25 distinct layers identified (24 fully fixed, 25 half-fixed)
- gdb-cheri toolchain working in QEMU guest
- Complete empirical Morello cap-ISA encoding reference (docs/27)
- Full diagnostic recipe (docs/30)

The JVM now runs **80+ instructions of interpreter prologue** before
hitting L25. We have crossed the first runtime helper call site.

## Next-session move

For L25 specifically: implement Option 3 (pass entry_point cap from
C++ via dedicated register), since it's the cheapest and the
infrastructure is local to call_VM_base.

Approach:
```cpp
// In call_VM_base callers (C++ side), set a dedicated register
// (say c19 — currently a spare callee-saved) to the entry_point
// cap *before* the stub dispatch.
//
// In stub-emitted call_VM_leaf_base, replace
//   adrp + cap-ADD + blr
// with
//   blr c19  // entry_point passed in c19
```

Once L25 lands, expect more L-style layers (each call/jump site that
crosses code cache ↔ libjvm.so boundary). Estimate 5-10 more layers
before `java -version` exits 0.

## Strategic implication for paper §2

Every CHERI Morello HotSpot port will hit this code-cache-vs-runtime
boundary problem. It's not specific to StoplessGC. It's a foundational
issue with HotSpot's "JIT-and-stub-generated code calls back into
C++ runtime" architecture under CHERI.

Our StoplessGC's CHERI-native design SIDESTEPS this entire family of
problems because we don't generate stubs that need to call runtime
helpers. Our handler is pure cap_runtime C code; the JVM just signals
us via SIGPROT and we resume via existing mcontext.

This is a paper §2 argument no other paper makes — quantitatively,
because nobody else has documented the per-layer cost of porting
HotSpot to CHERI Morello.
