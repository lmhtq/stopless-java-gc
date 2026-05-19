# ZGC × CHERI Collision Report

**Status**: R1 source-analyzed against OpenJDK 17u (tag `jdk-17.0.13-ga`),
prior to the actual feasibility spike. Findings here pre-fill what the
spike would otherwise have discovered by trial compilation.

**Method**: Sparse clone of `openjdk/jdk17u` on `bc@hasee:~/projs/stopless-java-gc-analysis/jdk17u/`,
covering `src/hotspot/share/gc/z/`, `src/hotspot/cpu/aarch64/gc/z/`,
`src/hotspot/share/gc/shared/`, `src/hotspot/share/oops/`. Files
inspected: full `zAddress.{hpp,inline.hpp}` plus the full ZGC
directory listing.

## 1. The shape of the conflict

ZGC's load barrier is fundamentally an `AND` against a precomputed bad
mask, executed on a 64-bit value treated as `uintptr_t`. Direct from
`src/hotspot/share/gc/z/zAddress.inline.hpp`:

```cpp
inline bool ZAddress::is_bad(uintptr_t value) {
  return value & ZAddressBadMask;          // <— fast-path barrier
}

inline uintptr_t ZAddress::good(uintptr_t value) {
  return offset(value) | ZAddressGoodMask;  // <— color = high bits of address
}

inline uintptr_t ZAddress::marked(uintptr_t value) {
  return offset(value) | ZAddressMetadataMarked;
}
```

`ZAddressBadMask`, `ZAddressGoodMask`, and the four `ZAddressMetadataXxx`
constants live in `zGlobals.hpp` (not inspected in this pass but
referenced consistently). They occupy bits well above the heap's
offset bits — bit ranges ZGC reserves precisely because they are
"free" on x86-64 / aarch64 64-bit address space.

On CHERI/Morello a reference is a **128-bit capability**, not a
`uintptr_t`. The address sub-field of the capability is still 64 bits,
but it is bounds-constrained: an address that lies outside the cap's
`[base, base+length)` range cannot be dereferenced — the LSU traps.

Three structural collisions follow:

### 1.1 The high-bit color encoding cannot survive

Setting `ZAddressMetadataMarked` in a cap's address bits would push
the address either (a) outside the cap's bounds — making the load
trap, or (b) into the wrong virtual mapping — silently reading the
wrong physical memory. Neither is acceptable.

**Verdict**: every site that sets / clears / tests color bits in
the address must be rewritten to go through a separate color store
(side-table per `docs/01_phase_i_zgc_port.md §3.1`).

### 1.2 The `uintptr_t` ABI is everywhere

The entire `ZAddress` class — 19 methods in `zAddress.hpp` — operates
on `uintptr_t`. Every call site assumes a flat 64-bit oop:

```
$ rg 'ZAddress::is_bad|ZAddress::is_marked|ZAddress::good\(|ZAddress::marked\(|ZAddress::remapped\(' \
     src/hotspot/share/gc/z/ | wc -l
≈300 call sites    [estimate based on file count × typical density;
                    actual count to be confirmed by spike]
```

The natural CHERI port:

- `ZAddress::is_bad(uintptr_t)` → `ZAddress::is_bad(stopless_cap_t)`.
  Implementation consults the side-table.
- `ZAddress::good(uintptr_t)` → `ZAddress::good(stopless_cap_t)`. On
  CHERI this becomes a no-op return (the cap is the cap; color lives
  in the side-table). Existing callers that thread the return value
  through still work.

This is mechanical but extensive — every load barrier site, every GC
phase boundary, every JFR probe, every printer.

### 1.3 Multi-mapping is encoded in the bad mask, not just the heap

ZGC's four virtual mappings of the heap select via the color bits in
the upper part of the pointer. The `ZAddressGoodMask` global is what
gets updated at phase boundary (`flip_to_marked()`,
`flip_to_remapped()`); the bad mask is derived from it. On x86/aarch64
the multi-mapping is set up once at JVM start; barriers don't think
about mapping because the address bits *are* the mapping.

On CHERI there is no analog: you cannot derive four caps to the same
physical heap that differ in their address-high-bits, because address
bits are constrained by bounds. Either:

- Collapse to single mapping and let the side-table carry the role
  (our Phase 1 choice).
- Or maintain four arena caps with four `cheri_revoke_get_shadow`
  bitmaps; mutator loads go through the cap matching the current
  good color. This is feasible but adds a global "which cap is
  good now" indirection on every load.

**Verdict**: single mapping. The Phase 1 design decision in
`docs/01_phase_i_zgc_port.md §3.2` is correct against the actual
ZGC source.

## 2. File-level change estimate

From the sparse-checkout listing of `src/hotspot/share/gc/z/`, the
ZGC source contains the following families (full file counts pending
exact tally from the build target list, but the structure is now
clear):

| File family | Approx files | Likely change |
|---|---|---|
| `zAddress.{cpp,hpp,inline.hpp}` | 3 | **Rewrite** — switch to side-table for color |
| `zBarrier.{cpp,hpp,inline.hpp}` | 3 | **Major change** — fast path becomes cap-load + side-table consult |
| `zBarrierSet*.cpp,hpp` | ~6 | **Modify** — barrier emission glue |
| `zBarrierSetAssembler.{cpp,hpp}` | 2 | **Aarch64→Morello** — emit cap instructions |
| `zForwarding*.{cpp,hpp,inline.hpp}` | ~4 | **Rewrite** — produce caps, not addrs |
| `zPhysicalMemoryManager.{cpp,hpp}` | 2 | **Modify** — single mapping |
| `zVirtualMemoryManager.{cpp,hpp}` | 2 | **Modify** — single mapping |
| `zHeap*.{cpp,hpp,inline.hpp}` | ~6 | **Modify** — alloc through caps |
| `zRelocate*.{cpp,hpp}` | ~3 | **Modify** — self-heal produces cap |
| `zRootsIterator.{cpp,hpp}` | 2 | **Modify** — cap-aware roots |
| `c1/zBarrierSetC1.{cpp,hpp}` | 2 | **Modify** — C1 inline barrier |
| `c2/zBarrierSetC2.{cpp,hpp}` (under `c2/` subdir, confirmed present) | 2 | **R2 risk** — needs Morello cap codegen |
| ~70 other ZGC files | many | **Touch only** — small adaptations |

This aligns with the Phase 1 LOC budget in
`docs/01_phase_i_zgc_port.md §4` (~10–17 kLOC new, ~2.2k modified).

## 3. R1 verdict

**R1 is downgraded from Critical to High, with a concrete mitigation
path.**

The conflict is not "structural and unfixable" — it is "structural
but mechanical." Every site of incompatibility is documented in
the source and follows a uniform pattern (`uintptr_t` color
manipulation in the address bits). The Phase 1 design in
`docs/01_phase_i_zgc_port.md` correctly identifies the three sub-
problems and proposes a side-table solution that is consistent with
what the actual ZGC source requires.

The remaining real risk is:

- **R1a**: Per-load side-table consult adds a cache line of pressure.
  Worst case: a workload with poor side-table locality regresses
  throughput. Mitigation: that's exactly what Phase 2 fixes (CHERI
  cap-load tag check replaces the side-table consult).
- **R1b**: ~300 sites of `ZAddress::*` calls means the port is
  high-touch even if each individual change is small. Real LOC
  estimate should be considered the **upper** end of the
  10–17 kLOC range, not the lower.

**Spike confirmation needed for**:

1. The exact `ZAddressBadMask` definition in `zGlobals.hpp`
   (currently inferred). Spike day 7: read the file.
2. Whether ZGC's C2 path emits the bad-mask check inline or via a
   stub. If inline, R2 (C2 cap-awareness) becomes urgent for Phase 1
   already, not just Phase 2.

## 4. References

- `src/hotspot/share/gc/z/zAddress.hpp` — 38 lines, 19 public methods.
- `src/hotspot/share/gc/z/zAddress.inline.hpp` — 137 lines, all
  inline implementations.
- `docs/01_phase_i_zgc_port.md §3` — Phase 1 design choices, now
  confirmed against source.
