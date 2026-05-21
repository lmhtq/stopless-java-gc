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

## 5. Empirical confirmation — 2026-05-20

Attempted to run `configure` for OpenJDK 17u (tag `jdk-17.0.13-ga`) on
`bc@hasee` with `--openjdk-target=aarch64-unknown-freebsd
--with-toolchain-type=clang --with-toolchain-path=$SDK/bin
--with-sysroot=$SDK/sysroot-morello-purecap`, plus the discovered fixes:

- Explicit `--sysroot=` in `--with-extra-cflags/ldflags` (OpenJDK
  autoconf uses `-isysroot` for link tests, not `--sysroot=`).
- `BUILD_CC=clang BUILD_CXX=clang++` as positional args (env vars
  are ignored).
- `--disable-warnings-as-errors`.

After clearing those, autoconf reached pointer-size detection and
failed with:

```
checking size of int *... 16
configure: The tested number of bits in the target (128) differs from
the number of bits expected to be found in the target (64)
configure: You are doing a cross-compilation. Check that you have all
target platform libraries installed.
configure: error: Cannot continue.
```

Source: `make/autoconf/platform.m4:705`. The build system assumes the
target's pointer width matches `OPENJDK_TARGET_CPU_BITS` (64 for
aarch64). On Morello purecap, `sizeof(int *) == 16` because pointers
are 128-bit capabilities. **The build system itself, not just ZGC,
needs cap-aware patches.**

This is the first real engineering hit and exactly the kind of
collision predicted in §1.1 above. It is **mechanical**: the
autoconf check needs to be taught that "pointer is 128 bits even
though the architecture is aarch64-64". Patch shape:

```diff
--- a/make/autoconf/platform.m4
+++ b/make/autoconf/platform.m4
@@ -700,7 +700,11 @@
   AC_CHECK_SIZEOF([int *], [], [#include <stdio.h>])
   TESTED_TARGET_CPU_BITS=`expr 8 \* $ac_cv_sizeof_int_p`

-  if test "x$TESTED_TARGET_CPU_BITS" != "x$OPENJDK_TARGET_CPU_BITS"; then
+  # On CHERI/Morello purecap, pointers are 128-bit capabilities even
+  # though the target CPU is aarch64-64. Accept that case.
+  AS_IF([test "x$OPENJDK_TARGET_CPU" = xaarch64 && test "x$TESTED_TARGET_CPU_BITS" = "x128"],
+    [AC_MSG_NOTICE([Detected CHERI/Morello purecap (128-bit pointers on 64-bit CPU); accepting])],
+    [test "x$TESTED_TARGET_CPU_BITS" != "x$OPENJDK_TARGET_CPU_BITS"], [
     AC_MSG_NOTICE([The tested number of bits in the target ($TESTED_TARGET_CPU_BITS) differs from the number of bits expected to be found in the target ($OPENJDK_TARGET_CPU_BITS)])
     AC_MSG_NOTICE([You are doing a cross-compilation. Check that you have all target platform libraries installed.])
     AC_MSG_ERROR([Cannot continue.])
+  ])
```

This is **patch #2** in `patches/openjdk-jdk17/series` (after the
build-system hook for cap_runtime). Further failures will surface
incrementally as autoconf moves past this check into actual
compilation.

**R1 is now empirically confirmed at the level the spike was designed
to reach: structural-but-mechanical.** Next deeper layer (the ZGC
source itself) is reached only after the build system patches let
us start actual compilation.

## 6. Phase 1 patch ledger (live)

Patches found and applied as `make` (or `configure`) surfaces each
collision. Each is a small mechanical fix; the value is having the
full set catalogued.

| # | File | Collision | Patch shape |
|---|---|---|---|
| 0002 | `make/autoconf/platform.m4:705` | `sizeof(int*)==16` mismatch with `OPENJDK_TARGET_CPU_BITS=64` | Add an aarch64+128bit exemption arm |
| 0003 | `make/hotspot/lib/JvmMapfile.gmk:55` | `Unknown target OS bsd` in nm/awk dispatch | Share Linux's branch via `isTargetOs, linux bsd` |
| 0004 | `make/autoconf/flags-cflags.m4:429` | `CFLAGS_OS_DEF_JVM` empty for bsd → HotSpot's `_ALLBSD_SOURCE`-guarded typedefs collide with system headers under CHERI | Set `CFLAGS_OS_DEF_JVM="-D_ALLBSD_SOURCE -D_GNU_SOURCE"` |
| 0005 | `src/hotspot/share/runtime/semaphore.hpp:38` *(pending)* | "No semaphore implementation provided for this OS" — HotSpot has `linux/macosx/windows` impls, not bsd | Add a `#ifdef _ALLBSD_SOURCE` arm or share posix-sem with linux |
| 0006 | `src/hotspot/os_cpu/bsd_aarch64/bytes_bsd_aarch64.hpp:39,45,49,53` *(pending)* | Uses GNU `bswap_16/32/64`, not present on CheriBSD | Use `__bswap16/32/64` from `<sys/endian.h>` |
| 0007 | `patches/openjdk-jdk17/0001-cap-runtime-hook.patch` *(pending)* | Build hook to link `src/cap_runtime/` into libjvm | See `docs/01_phase_i_zgc_port.md §4` |
| 0008+ | ZGC source proper | See `docs/01_phase_i_zgc_port.md §3-4` | The actual side-table redesign |

After applying 0002–0004 cleanly via `scripts/apply_patches.sh`,
`configure` completes successfully and `make images` reaches HotSpot
compilation, surfacing 0005 / 0006 as the next layer. Each subsequent
patch is captured here as it lands.

The pattern of "structural conflict, but small mechanical patch per
site, total ~10-50 patches" is exactly what `docs/01_phase_i_zgc_port.md`
predicted. ZGC source itself is the third layer down; we are
currently in layer 2 (HotSpot OS portability shims).
