# Phase 1 — ZGC Port to CHERI/Morello

## 1. Goal

Make OpenJDK 17's ZGC build and run correctly on CheriBSD / Morello (FVP
first, real Morello later if available). **No new GC mechanism is
introduced in Phase 1** — every behavior should be semantically equivalent
to upstream ZGC. The contribution is the design choices needed to
reconcile ZGC's pointer encoding with CHERI's immutable, bounded
capability model.

## 2. ZGC primer (what we are reconciling with)

ZGC uses three structural features that all interact with CHERI:

1. **Colored pointers.** Every reference is a 64-bit `oop` whose top
   bits encode GC state. Bits 42–45 (configurable) hold one of:
   `marked0`, `marked1`, `remapped`, `finalizable`. The fast load
   barrier ANDs the loaded oop with a "bad mask"; if any bad bit is set
   the slow path is taken.
2. **Multi-mapping.** ZGC creates four `mmap`s of the same physical heap
   pages at four different virtual address ranges. Each color value
   maps the pointer's address bits to a different virtual address.
   The hardware MMU does the disambiguation. Color flipping costs only
   a bitmask on the oop, not an actual memory copy.
3. **Forwarding tables and concurrent relocation.** During relocation,
   evacuated regions install a forwarding table mapping each from-space
   address to its to-space address. The load barrier slow path consults
   the table and rewrites the loaded oop ("self-healing"). When the
   relocation phase finishes, color is flipped so old caps trap on next
   load.

CHERI's constraints that bite each of these:

- **Tag + bounds + permissions are immutable per cap.** You can derive a
  narrower cap from a wider one (`csetbounds`), but you can't rewrite an
  existing cap in place. Self-healing must produce a *new* cap.
- **Caps must be loaded with cap-aware instructions (LDR Cx / STR Cx).**
  Any `ldr Xn` of a memory location that contains a cap loses the tag.
- **Address bits are real address bits.** You can't stuff GC state into
  the high bits of cap.address; bounds checks will fail or, worse,
  succeed at the wrong location.
- **Capability tag is per-128b granule.** A cap is 128 bits. The tag bit
  lives outside the addressable representation, controlled by hardware.

## 3. Design choices

### 3.1 Colored-pointer encoding

Three candidates; we evaluate each in the spike (S2):

| Option | Where color lives | Pros | Cons |
|---|---|---|---|
| **A: Side-table** | Per-object color in a shadow bitmap addressed by object address | No format change to cap; clean separation | Every barrier consults side-table → extra load; cache pressure |
| **B: Cap permissions** | 2 free permission bits in cap | Hardware-checked at every load (free!) | Permissions can only narrow under `candperm`; flipping color = synthesizing a new cap; only 4 color states fit |
| **C: Sealed caps / `otype`** | `otype` field, 16+ bits | Lots of space; flipping = `cseal`/`cunseal` | Sealed caps cannot be dereferenced — defeats the purpose for in-flight pointers |

**Decision (tentative): Option A side-table.** Option B is alluring but
requires every color-flip to mint new caps for every reference in the
heap — exactly the immutable-cap problem in miniature. Option C is
fundamentally incompatible with ZGC's hot path (sealed caps are
unreadable). Side-table costs a load per barrier; that load is L1-hot
in steady state.

The Phase 2 design *recovers* hardware-level color checking by using
Cornucopia Reloaded's per-page revocation bitmap as the side-table. So
Option A in Phase 1 is not throwaway — it becomes the platform for
Phase 2.

### 3.2 Multi-mapping

Two candidates:

| Option | Approach | Cost |
|---|---|---|
| **A: Multi-mapping preserved, cap derivation per color** | Maintain 4 mmaps; derive 4 root caps; each oop is a cap derived from the color-matching root | Heap virtual address space is fixed at JVM start; bounds-derive at allocation time only |
| **B: Single mapping + side-table** | Collapse to one mmap; color lives in the side-table from 3.1 | No special cap derivation; loses the "color-flip is a TLB-level operation" trick |

**Decision (tentative): Option B.** Once color lives in a side-table for
3.1, multi-mapping is redundant. The TLB-flip trick is a perf
optimization for color flip, which is rare (per-GC-cycle). We lose
some color-flip throughput; we gain a much simpler cap derivation
story.

### 3.3 Forwarding table

ZGC's forwarding table maps `from_addr -> to_addr` (uintptr_t pairs). On
CHERI we need `from_cap -> to_cap`. The from-side can stay address-keyed
(we hash by cap.address). The to-side must be a real cap with correct
bounds.

Two options for synthesizing to-caps:

- **A: Heap-rooted derivation.** Keep a root cap covering the entire Java
  heap. Each forwarding-table lookup returns a cap derived from the root
  by `csetbounds` to the new object's extent.
- **B: Per-region rooted derivation.** Each ZGC region has its own root
  cap. Forwarding-table entries are derived from the destination region's
  root.

**Decision (tentative): Option B.** Tighter bounds, better fault
isolation, and aligns with ZGC's existing region model. Cost: one extra
indirection (region table) per forwarding lookup, in the slow path only.

### 3.4 Barrier asm (`cpu/morello/gc/z/`)

A new HotSpot CPU subdirectory mirrors `cpu/aarch64/gc/z/` but emits
Morello capability-mode instructions:

- `LDR Cx, [Cy, #off]` instead of `LDR Xn, [Xm, #off]` for reference
  loads.
- Slow-path stub at a fixed offset; trap on cap-tag-clear via the
  Cornucopia revoke mechanism (Phase 2 only — Phase 1 keeps software
  bad-mask check).

In Phase 1 the asm is mostly mechanical translation. The semantic
substitution happens in Phase 2.

### 3.5 Allocator

`ZAllocator` (TLAB and slow path) must produce caps with correct bounds
for each allocated object. Today it returns `oop` (raw 64-bit). Adapt to
return cap; bounds = `[obj_start, obj_start + size)`. Hooks into
`CollectedHeap::array_allocate`, `obj_allocate`, and TLAB refill.

### 3.6 Root scanning

Thread stacks, JNI handles, and JVMTI roots all hold caps under CHERI.
During relocation:

- Stop the threads (existing ZGC root scan does this for a brief
  STW window; we don't change that semantics in Phase 1).
- For each cap on a root, look up in forwarding table; mint replacement
  cap.

No changes to *when* root scanning happens; only changes to *what it does
per root*.

### 3.7 C2 JIT integration

ZGC's fast-path barrier is emitted directly by C2 as inline asm. Morello
C2 status is the #1 risk in `04_risk_register.md`. If C2 emits
capability-aware instructions correctly, we override
`BarrierSetC2::load_at` to emit `LDR Cx`. If not, we fall back to a
stub call in Phase 1 (slow), and tackle C2 directly in a Phase 1.5
patch if needed.

## 4. Module breakdown (LOC budget)

| Module | New LOC | Modified LOC |
|---|---|---|
| Side-table (color storage) | 800 | 0 |
| Cap-aware forwarding table | 1200 | ~300 in `ZForwarding(Table)` |
| ZHeap / virtual-memory manager (collapse multi-mapping) | 1500 | ~800 in `ZVirtualMemoryManager` / `ZPhysicalMemoryManager` |
| `cpu/morello/gc/z/` (new dir) | 2500 | 0 |
| Root scanning cap-awareness | 1000 | ~400 in `ZRootsIterator` |
| `ZAllocator` cap-bound derivation | 700 | ~150 in `ZAllocator`, `ZHeap::alloc*` |
| C2 barrier integration (or stub-fallback) | 1000 | ~300 in `g1/zBarrierSetC2.cpp` analogues |
| `ZRelocate` cap-aware self-healing | 600 | ~200 |
| Cap-aware unit + jtreg tests | 800 | 0 |
| cheribuild integration / build glue | 400 | 0 |
| **Total** | **~10.5k** | **~2.2k** |

Range: 10–17k LOC new (lower bound if Option B for both 3.1 and 3.2;
upper if we hit C2 issues and need fallback infra).

## 5. Testing strategy

| Layer | What we test | Tool |
|---|---|---|
| Unit | Side-table get/set, forwarding-table cap synthesis, region-root derivation | gtest, `tests/unit/` |
| HotSpot internal | ZGC stress tests adapted for cap-aware semantics | jtreg subset, runs in Morello FVP |
| Java workload smoke | DaCapo `h2` and `pmd` complete | `tests/integration/` |
| Cap-discipline | No untagged loads in ZGC fast path (verified by Morello FVP cap-trap logging) | FVP capability-trap counter |
| Cap-leak | Forwarding never returns a cap with bounds wider than the destination region | gtest + invariant assertion |

Phase 1 passes when DaCapo `h2` runs to completion on Morello FVP with
ZGC enabled and zero unexpected cap traps.

## 6. Workshop paper outline

```
Title: Porting ZGC to CHERI/Morello — design choices for moving GC
       on immutable capabilities

§1 Introduction
   - ZGC overview, CHERI overview, the immutable-cap problem
§2 Background
   - Colored pointers, multi-mapping, forwarding table
   - CHERIvoke / Cornucopia / Cornucopia Reloaded
   - MOJO's G1/Serial port
§3 Design space
   - Colored-pointer encoding (side-table vs. perm vs. otype)
   - Multi-mapping vs. single-mapping
   - Per-heap vs. per-region cap derivation
§4 Implementation
   - HotSpot module breakdown
   - Build pipeline (cheribuild + cheribsd + JDK)
§5 Evaluation
   - DaCapo / Renaissance smoke results on Morello FVP
   - Compare against MOJO G1 baseline (different GC, but same platform)
   - Microbench: bare cap-load vs. cap-load + side-table lookup
§6 Lessons & limitations
   - C2 integration challenges
   - What we could not do without ISA additions
§7 Related work, Conclusion
```

Target venues: VMIL @ SPLASH, ICOOLPS @ ECOOP, MARC, DSbD annual. arXiv
preprint goes up the day we submit.
