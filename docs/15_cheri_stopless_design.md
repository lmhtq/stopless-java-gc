# CHERI-Stopless: hardware-accelerated truly pauseless GC

**Status:** design draft, paper §3 candidate
**Date:** 2026-05-27
**Authors:** bluecat

This document specifies the core mechanism of *CHERI-Stopless*, a
moving garbage collector for the JVM that uses CHERI capability
hardware to achieve a STW-pause time of **zero** while keeping per-
load mutator overhead to a **single hardware-checked instruction**.

The design re-uses the lock-free invariants of Stopless GC (Pizlo,
Petrank, Steensgaard, ISMM 2007) but replaces its software CAS-cell
forwarding with CHERI capability revocation + per-mutator fault
handling, drawing technique inspiration from Cornucopia Reloaded
(MSR 2024) but applied to *moving* GC rather than revocation-only.

## 1. Stopless GC, restated

Stopless GC (2007) achieves a moving collector with no STW pause via
three invariants:

* **MI (Mutator Invariant)**: every oop dereference observes either
  (a) the live object at its original location, or (b) a forwarded
  reference to the moved object. Never both. Never a partial state.

* **CI (Collector Invariant)**: when the collector moves object `O`
  from address `A` to address `B`, all mutator references to `O`
  see the move atomically. Either the mutator sees `O@A` and races
  with the collector (which has to retry), or sees `O@B` directly.

* **NS (No-Stop)**: neither mutator nor collector ever blocks the
  other. The collector races against potentially-mutating mutators;
  the mutators race against the collector copying their pointed-to
  objects.

Stopless implements these via:
1. Indirection cells: each oop is reached through a "wide cell"
   that contains both the object payload and a "forwarding" slot.
2. **CAS** on the wide cell to install forwarding pointers.
3. Mutator load is a load + check forwarding slot + CAS retry
   (typically 3-5 instructions on the fast path).

The cost of MI+CI under Stopless is the **per-access CAS check**
which is software-emitted on every read.

## 2. The CHERI primitive we exploit

ARM Morello / CHERI capabilities are 128-bit values with a
hardware-managed **tag bit**:

* The tag is set when the capability is constructed validly (e.g.,
  from a wider cap via `csetbounds`).
* The tag is cleared when the cap's referenced memory is *revoked*,
  via the FreeBSD `cheri_revoke` syscall (Cornucopia Reloaded
  mechanism). Revocation runs concurrently with mutators; it sweeps
  memory looking for caps to the revoked region and clears their
  tags atomically per word.
* Any load or store through a tag-zero capability triggers a
  hardware trap (`SIGPROT` with `si_code = PROT_CHERI_TAG`).

The key fact we leverage:

> **Cap tag check is a single instruction** (the load itself).
> No software check is needed; the hardware enforces it.

This gives us a sub-instruction-cost barrier compared to Stopless's
multi-instruction CAS-check. The cost is paid only when a stale
capability is actually loaded — i.e., when forwarding is needed,
which is rare.

## 3. CHERI-Stopless design

### 3.1 Heap layout

The heap is divided into **regions** of 2 MiB each (matching ZGC's
chunk size for engineering convenience, though the design is region-
size-agnostic). Each region is one of:

* `FREE` — uncommitted
* `LIVE` — contains live objects, normal mutator access
* `EVACUATING` — collector is currently copying its contents
  elsewhere; mutator access still allowed but may trigger forwarding
* `EVACUATED` — collector has copied all contents out; pending
  revocation
* `REVOKED` — all caps to it have been invalidated; ready for reuse

Each region is backed by an mmap'd file-backed or anon mapping with
exactly the region's bounds. The **region cap** has bounds
`[base, base+2MiB)` and is the source of all sub-caps to objects in
the region.

### 3.2 Object reference is always a cap

Every oop in the JVM heap (or on a thread's stack, or in a register)
is a CHERI capability with:

* address = object's start in its current region
* bounds = `[obj_start, obj_start + obj_size)` (tight per-object
  bounds, via `csetbounds` at allocation)
* tag = 1 if live, 0 if revoked

Per-object bounds give us "free" object-bounds checking — a load
past the end of an object hardware-traps without any software
check. This subsumes ZGC's region-bounds + object-bounds barriers.

### 3.3 The collector loop

```
for each region R in EVACUATION_SET:
    R.state = EVACUATING
    for each live object O in R:
        # Phase A: allocate to-space copy
        B = allocate_in(some_TARGET_region, sizeof(O))
        # Phase B: copy contents (cap-tag preserving via memcpy_caps)
        memcpy_caps(B, O.addr, O.size)
        # Phase C: install forwarding table entry
        forwarding_table.insert(O.addr → B)
    # Phase D: revoke all caps to this region
    cheri_revoke(R.base, R.size)
    R.state = EVACUATED → REVOKED
    R.state = FREE   # reuse
```

The collector **never stops the world**. Mutators continue running
during Phases A-D.

### 3.4 The mutator load (fast path)

```asm
    ldr  c0, [c_obj, #field_offset]    # single cap load
    # tag of c0 reflects validity:
    #   tag=1: c0 points to valid object, use directly
    #   tag=0: hardware traps to SIGPROT handler
```

The mutator's read of an oop field is **a single hardware
instruction**. No software check, no AND, no CBNZ, no
multi-mapping.

### 3.5 The SIGPROT handler (slow path)

```c
void cheri_stopless_handler(int sig, siginfo_t* si, void* ctx) {
    // 1. Identify the faulting capability and its source slot.
    void* faulting_cap_value = si->si_addr;
    uintptr_t source_slot     = decode_source_slot(ctx);
    
    // 2. Look up the forwarding table.
    void* forwarded_cap = forwarding_table.lookup(faulting_cap_value);
    if (forwarded_cap == NULL) {
        // True dangling: re-throw as JVM exception
        // (in CHERI-Stopless, the collector never frees memory
        // mutators legitimately hold, so this means a use-after-free
        // bug in JNI native code or similar.)
        raise_jvm_error();
        return;
    }
    
    // 3. Rematerialize the cap from the forwarding target's
    //    region cap (DDC-derived in practice).
    void* new_cap = rebuild_cap(forwarded_cap, /*bounds:*/ object_size);
    
    // 4. Self-heal: store the new cap back to the source slot.
    //    Future loads from the same slot will succeed without faulting.
    *(void**)source_slot = new_cap;
    
    // 5. Set the destination register of the faulting load
    //    to the new cap, then resume at the next instruction.
    set_ucontext_register(ctx, faulting_dest_reg, new_cap);
    advance_pc(ctx, INSTRUCTION_SIZE);
    return;
}
```

The handler is per-mutator (signal handlers are per-thread on
FreeBSD); no global lock is needed. The forwarding table is
read-mostly + occasionally appended-by-collector, so a
read-optimised structure (e.g., RCU hash map or split-ordered list)
suffices.

### 3.6 Concurrent move correctness

The key race is: collector is copying `O@A → O@B`. Mutator writes
`O@A.f = X` concurrently. After copy completes, `B.f` lacks `X`.

**Solution (write barrier on EVACUATING regions only)**: while a
region is `EVACUATING`, its region cap has *read-only* permissions
for mutators. Mutator stores trap to SIGPROT handler which:

1. Look up object's forwarding entry. If exists, redirect store to
   the new location `B.f = X`.
2. If not yet forwarded, place the store in a per-region pending
   queue; collector applies the queue after copying.

Mutator **reads** don't need this — they always see either the old
object (tag still valid until revocation) or fault (after
revocation, handler forwards).

### 3.7 Comparison

| | Stopless 2007 | ZGC 2018 | CHERI-Stopless |
|---|---|---|---|
| Pause time | 0 | sub-ms | **0** |
| Per-load fast-path cost | 3-5 insns (CAS) | 3 insns (AND+CBNZ+br) | **1 insn (cap-load)** |
| Forwarding mechanism | wide cell + CAS | offset table + color | **cap revocation + handler** |
| Region check | software mask | software mask | **hardware bounds** |
| Hardware required | none | none | CHERI |
| Move concurrency | full | concurrent + brief STW | full |

## 4. Open design problems

### 4.1 Forwarding table scalability

Per-object entries explode memory. Hierarchical structure:

* **Level 1**: per-region forwarding map (entry only if region is
  in `EVACUATING` or `EVACUATED` state). After region is `REVOKED`
  and reused, entry is dropped.
* **Level 2**: within an EVACUATING region, per-page bitmap
  identifies pages that have been moved; per-page offset table
  gives per-object forwarding within the page.

Memory cost: `O(regions_being_collected × pages_per_region)`. For
typical heap and EVACUATION_SET sizes, < 1% of heap size.

### 4.2 Per-thread cap forwarding cache

Trap cost is ~1-10 μs (signal delivery + kernel + handler). At trap
rates above ~10k/sec, this dominates. Mitigations:

* **Self-healing** (already in §3.5): subsequent loads from the
  same slot don't trap.
* **Per-thread LRU cache** of recently-rematerialized caps:
  `(old_cap_address → new_cap)`. Handler checks cache before global
  forwarding table.
* **Lazy revocation**: collector batches `cheri_revoke` calls,
  amortizing kernel cost across many objects.

### 4.3 Pointers from cap-typed JVM-internal metadata

`Klass*`, `Method*`, and friends are cap-typed under purecap (per
patch 0066). If we move Klass instances (we don't typically), the
forwarding mechanism must extend to these. For now, **metadata is
not moved** — only Java oops are subject to CHERI-Stopless GC. This
matches ZGC which doesn't move metaspace either.

### 4.4 Root scanning

Java threads have oop references in registers + stack frames.
Conventional GC scans roots while threads are stopped.

CHERI-Stopless avoids stopping threads via **per-thread root cap
sets**:

* Each thread maintains a small `RootSet` structure listing its
  current handle/local roots (updated on JNI entry/exit, etc.).
* Stack roots are discovered via the compiled-code oop maps as
  usual, but read **concurrently** with the mutator running —
  reads from the stack hit the mutator's snapshot if the mutator
  is at a safepoint poll, OR the collector tolerates inconsistency
  by re-scanning until quiescent.

Stack scanning may require brief STW at the start of GC for
consistency (~10 μs per thread). This is the only potential STW;
hand-waving claim of "zero pause" requires solving this.

**The fully pauseless story** uses on-stack snapshotting via cap
discipline: every mutator caches its current root cap set in a
TLS region, updated at safepoint polls. Collector reads from TLS
without stopping mutator. Reading slightly-stale roots is safe
because newly-allocated objects are conservatively assumed live
until the next cycle.

## 5. Implementation roadmap

Phases in order of dependency:

* **A**: `cap_runtime/` primitives — cap_revoke wrapper, forwarding
  table, signal handler skeleton, rebuild_cap helper. Standalone
  C++, testable on QEMU without JVM. 1-2 weeks.

* **B**: Standalone allocator + mover demo: allocate N objects,
  collector loop moves half, mutator reads via cap-load, verify
  faults+handler work end-to-end. 1 week.

* **C**: OpenJDK plugin (`src/hotspot/share/gc/stopless/`). Start
  from Epsilon as base (no existing GC complexity); add the move
  phase. Skip ZGC complexity entirely. 2-3 weeks.

* **D**: Measurement harness (CHERI-QEMU functional only):
  pause time (= 0 by construction), trap rate, per-load instruction
  count, forwarding-table memory cost. 1 week.

* **E**: Paper writing. 4 weeks.

Total: **~10 weeks** to draft submission.

## 6. What we DON'T claim

* Performance / cycle-accurate measurements — those require Morello
  silicon. We measure mechanism correctness + instruction counts +
  trap rates on QEMU.
* Production-grade GC throughput — this is a research prototype.
* That CHERI is the only way — Stopless 2007 already proved
  pauseless is possible; we add the *hardware* version.

## 7. Relation to prior work

* **Stopless GC** (Pizlo et al., ISMM 2007): software CAS-cell
  forwarding. We replace software with cap-tag hardware.
* **Cornucopia** / **Cornucopia Reloaded** (Wesley, Filardo et al.,
  MSR 2020, 2024): cap revocation for temporal safety. They revoke
  dead memory; we revoke moved memory and add forwarding.
* **ZGC** (Liden, OpenJDK 2018): colored pointers, multi-mapping,
  sub-ms pauses. We claim zero pause + simpler barrier.
* **MOJO / Soteria** (Nisbet, Wong et al.): OpenJDK on Morello
  purecap with Serial/G1. They don't address pauseless GC. We
  reuse their porting infrastructure (T_ADDRESS injected fields,
  16-byte HeapWord, etc.) and add the moving GC.
* **Shenandoah** (Flood, Kennke 2016): concurrent moving with
  Brooks pointers. Software indirection. We claim no per-object
  indirection — caps point directly at objects.

## 8. Open questions for review

1. Is per-object cap bounds prohibitive (one csetbounds per
   allocation)? Estimate: ~3-5 ns per alloc on Morello, comparable
   to existing per-alloc overhead.

2. Is `cheri_revoke` fast enough? Cornucopia Reloaded reports
   ~10ms revocation pass per GB; for our 2 MiB region the sweep
   would be ~20 μs, fast.

3. Concurrent move WRITE barrier (§3.6): does the per-page pending-
   queue actually scale? Need experiment.

4. Trap rate: what fraction of mutator loads actually fault during
   active GC? Best case 0 (all caps stable), worst case 100% (just
   after global revoke). The forwarding cache (§4.2) and lazy
   revocation determine the actual answer.

5. Stack-root snapshotting (§4.4) — TLS-cached root set is hand-
   wavy; needs careful design before claiming truly zero pause.
