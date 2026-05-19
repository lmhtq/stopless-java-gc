# Design Overview

## 1. Problem

Modern low-pause garbage collectors for the JVM (ZGC, Shenandoah) pay a
non-trivial throughput cost for sub-millisecond pauses. The cost is concentrated
in the **software load barrier** — every reference load executes a few cycles of
mask + branch to detect whether the pointer is "stale" (points into a region
being relocated). On highly pointer-chasing workloads the barrier can consume
5–15% of total CPU.

ARM's CHERI capability hardware (shipped in the Morello prototype CPU)
provides hardware-checked, tagged pointers with bounds, permissions, and a
revocation primitive. Every capability load is already hardware-checked.
Existing CHERI work (CHERIvoke, Cornucopia, Cornucopia Reloaded) has used
these primitives to implement **temporal safety for C/C++ heaps** — i.e.
reclaiming memory after `free()` without use-after-free. That work covers
*revocation* of capabilities. It does **not** cover *forwarding* —
relocating an object and rewriting all references to it, which is the core
mechanism of moving GCs like ZGC.

This project asks: **can CHERI's hardware load barrier replace ZGC's
software load barrier in a concurrent moving collector?**

If yes, the gain is a load barrier whose fast path is a normal capability
load — zero added cycles. The slow path (stale capability detected) traps
to a software handler that performs the forwarding. Mutator-visible pause
time stays at ZGC's level; per-load CPU cost drops.

## 2. Why this is open

The four sub-claims of the cross-domain idea decompose as:

| Claim | Prior art | Open? |
|---|---|---|
| Use CHERI capability revocation to reclaim memory | CHERIvoke (MICRO'19), Cornucopia (S&P'20) | No |
| Make revocation concurrent via per-page hardware load barriers | Cornucopia Reloaded (ASPLOS'24) | No |
| Run an OpenJDK GC on Morello / CheriBSD | MOJO project (Manchester+THG, 2024+) — Epsilon / Serial / G1 | No |
| **Use CHERI primitives as the load-barrier mechanism for a concurrent *moving* GC (ZGC-class)** | — | **Yes** |

The fourth slice is open because:

- Cornucopia-line work **clears** capability tags to invalidate stale caps.
  Java moving GCs **forward** them: an old cap to a from-space object must
  resolve to the new to-space location.
- CHERI capabilities are **immutable**: you cannot rewrite a cap's target in
  place; you can only invalidate it.
- MOJO does not port ZGC. ZGC's two hardest-to-port features —
  colored-pointer high-bit encoding and 4× virtual mapping — both collide
  with CHERI's pointer format.

This project addresses both halves: it (a) ports ZGC to CHERI and (b)
replaces the software barrier with a CHERI-native one. The two halves are
two papers.

## 3. Two-phase plan

### Phase 1 — ZGC port to CHERI/Morello

Goal: a buildable, runnable ZGC on CheriBSD / Morello FVP (and ideally real
Morello). No semantic redesign — just the engineering work to make ZGC
compatible with 128-bit capabilities, immutable cap targets, and CHERI's
load/store discipline.

The three hard sub-problems:

1. **Colored-pointer encoding.** ZGC uses the top 4–5 bits of a 64-bit
   pointer for GC state (marked0, marked1, remapped, finalizable). On CHERI
   the address field is constrained by the cap's bounds; high bits aren't
   free. Design choices: (a) side-table keyed by object address, (b)
   re-purpose cap permission bits, (c) use the cap's `otype` (sealed-cap
   mechanism). See `01_phase_i_zgc_port.md §3.1`.
2. **Multi-mapping.** ZGC creates four virtual mappings of the same physical
   heap; color bits in the pointer select which mapping. On CHERI all caps
   must be derivable from a parent capability; the four mappings would
   require four disjoint cap roots. Design choices: keep multi-mapping with
   cap-aware derivation, or collapse to single mapping + side-table
   indirection. See `01_phase_i_zgc_port.md §3.2`.
3. **Forwarding table.** ZGC's relocation phase consults a forwarding
   table to find an object's new address. On CHERI the entry must produce
   a *new capability* with the correct bounds. See
   `01_phase_i_zgc_port.md §3.3`.

Output: arXiv preprint #1 (workshop class — VMIL @ SPLASH, ICOOLPS,
DSbD workshop), GitHub repo v0.1, ~10–17k LOC new.

### Phase 2 — CHERI-native ZGC barrier

Goal: replace ZGC's software load barrier with a CHERI cap-load. The cap
itself encodes "this pointer is to a region being relocated" via Cornucopia
Reloaded's per-page revocation bit. A stale cap load traps; the trap
handler consults the (already-ported) forwarding table and returns a fresh
cap into to-space.

Falsifiable claim: **on Morello FVP, ZGC's per-load barrier CPU cost is
reduced by ≥3× on pointer-chasing workloads (DaCapo h2, pmd) with no
regression in p99.9 pause time vs. ported-but-unmodified ZGC from Phase 1.**

Output: arXiv preprint #2 (main paper — ASPLOS, ISMM, or PLDI submission),
GitHub repo v1.0, ~2.5–5.4k LOC new (Phase 1 infrastructure reused).

## 4. What is explicitly out of scope

- **Eliminating *all* GC pauses.** ZGC root scanning is already ~sub-ms; CHERI
  does not help thread-stack root scanning. We do not chase pauseless-root-
  scanning territory.
- **Generational ZGC.** Java 25's generational ZGC adds a young/old split.
  Phase 2 may target non-generational ZGC initially. Generational support
  is a Phase 3 follow-up if Phase 2 lands.
- **Real Morello board.** UK DSbD access has a months-long approval path.
  Phase 1+2 deliver on Morello FVP (cycle-approximate). Real-board numbers
  would be a Phase 3 follow-up.
- **CHERI-RISC-V.** Morello is aarch64-specific. CHERI-RISC-V is mentioned
  in related-work but is not a build target.

## 5. References to other docs

- `01_phase_i_zgc_port.md` — Phase 1 detailed design
- `02_phase_ii_cheri_barrier.md` — Phase 2 detailed design
- `03_build_setup.md` — bootstrap, build, run instructions
- `04_risk_register.md` — top risks and 2-week feasibility spike plan
