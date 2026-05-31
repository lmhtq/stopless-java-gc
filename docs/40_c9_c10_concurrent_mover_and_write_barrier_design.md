# C-9 / C-10 design: concurrent mover + CHERI cap-tag write barrier

Concrete, implementation-level design for the StoplessGC collector
(C-9) and the concurrent-move write barrier (C-10). Builds on the
high-level design in `docs/15_cheri_stopless_design.md` (MI/CI/NS
invariants, the cap-load fast path, the SIGPROT slow path) and on what
we have actually built and validated by 2026-05-31:

- the barrier **mechanism is proven end-to-end** on Morello
  (`cap_runtime/.../tests/test_basic`: move → forward_table insert →
  `cheri_revoke` → cap-load traps SIGPROT → handler forwards → resume,
  reading the correct value at the new location). See
  `[[barrier-mechanism-validated]]`.
- the **Zero route** is the main track: the C++ `BytecodeInterpreter`
  is clang-compiled, so every oop load AND store is a clang-emitted
  capability load/store. `java -XX:+UseStoplessGC -version` boots with
  the SIGPROT handler installed (C-7).
- current `StoplessHeap` = a single bump-pointer arena (C-3/C-4) with
  NOP barriers; `forward_table.c`, `revoke.c` (`cheri_revoke` + shadow
  bitmap), `handler.c` (reg-scan forwarder, self-heal) exist.

This doc fixes the design decisions docs/15 left abstract and records
where reality diverges from it.

---

## 0. The one big idea (and why Zero makes it stronger)

ZGC/Shenandoah put a **software load barrier** on every reference read.
CHERI-Stopless replaces it with the **hardware capability tag check**
that already happens on every `LDR Cn`. When the collector revokes an
object's old location, the mutator's stale cap goes tag-0; the next use
**traps** to our SIGPROT handler, which forwards.

Zero strengthens this: HotSpot's template interpreter would need a
hand-emitted barrier at every oop load *and* store site. In Zero the
interpreter is C++ — `oop x = obj->field` is a clang `ldr c`, and
`obj->field = x` is a clang `str c`. **Both** fault on a revoked cap.
So we get a **unified read+write hardware barrier for free**, where ZGC
has only a load barrier and bolts the store/marking barrier on in
software. That is a paper-worthy delta (docs/02 §2.2 + this).

```
   ZGC:   load  -> SW barrier (AND+CBNZ+br, ~3 insns)
          store -> SW marking/remember-set barrier (more insns)
   CHERI-Stopless on Zero:
          load  -> ldr c   (1 insn; tag-0 -> trap)   ┐ same hardware
          store -> str c   (1 insn; tag-0 -> trap)   ┘ check, both free
```

---

## 1. Region model (extend the single arena)

Current: one `stopless_arena_t` = one bump region. For evacuation we
need to move objects OUT of a region and reuse it, so we partition the
heap into fixed-size **regions** (e.g. 2 MiB), each its own
sub-allocation window inside the mmap'd arena, each with a state:

```
   FREE        empty, available for allocation / as evac target
   LIVE        mutator allocates here (bump); normal access
   EVACUATING  collector is copying objects out; being revoked/forwarded
   EVACUATED   all live objects copied out; awaiting revoke completion
   (then) -> FREE
```

A region carries: base cap, bump cursor, state, mark bitmap (live
objects), and — while EVACUATING/EVACUATED — a forwarding sub-map
(see §5). Allocation bumps within the current LIVE region; when full,
grab a FREE region. This is the standard region-based moving-GC layout
(ZGC pages, Shenandoah regions); CHERI only changes the barrier.

`StoplessHeap` (today a single bump arena) grows a `StoplessRegionTable`
indexing regions by address; `mem_allocate` allocates from the current
LIVE region; `is_in`/`block_start` consult the table.

---

## 2. C-9 — the collector

### 2.1 Thread

`StoplessCollectorThread : public ConcurrentGCThread` (HotSpot's
concurrent-GC thread base, already used by ZGC/Shenandoah). It runs the
collector loop off the mutator threads; `gc_threads_do` returns it.
Triggered by: heap occupancy watermark (allocation in `mem_allocate`
posts a request), or `System.gc()` (`collect()`), or periodic.

### 2.2 The cycle

```
collect_cycle():
  1. mark:    concurrent mark from roots (C-8 root scan) -> live bitmap
              per region. (Roots: Threads::oops_do + OopStorageSet +
              CLDG + Universe; on Zero, thread stacks = ZeroStack
              interpreterState frames.)
  2. select:  EVACUATION_SET = regions with low live-occupancy
              (best reclaim per byte moved).
  3. evacuate (per region R in set):
        R.state = EVACUATING
        # REVOKE-FIRST (see §3): clear all mutator caps to R up front
        cheri_revoke(R.base, R.size)        # mutator access to R now traps
        for each live O@A in R:
            B = alloc_in(target FREE region, O.size)
            memcpy_caps(B, A, O.size)        # tag-preserving copy
            forward_table.cas_insert(A.addr -> B)   # idempotent; mutator
                                                    # may race (see §4)
        R.state = EVACUATED
  4. (lazily) once no mutator can hold an A-cap (next revoke epoch
     passes), drop R's forwarding entries and R.state = FREE.
```

### 2.3 REVOKE-FIRST vs copy-first (the key decision)

docs/15 §3.3 copied first then revoked. We choose **revoke-first**:
revoke R *before* copying its objects. Rationale:

- After revoke, **every** mutator access to R traps — reads and writes.
  So the concurrent-move write race (docs/15 §3.6) is subsumed: a write
  to A after revoke can't silently land in stale A; it traps and the
  handler redirects it (§4). No separate "read-only EVACUATING phase"
  is needed — and CHERI has no cheap "downgrade all caps to RO"
  primitive anyway (revoke clears the tag, it doesn't strip W), so
  docs/15 §3.6's read-only model isn't directly implementable; revoke-
  first replaces it.
- The collector copies using a **privileged copy cap** to R derived
  from the arena root (not subject to the mutator-visible revocation),
  so it can still read A after revoke. (Engineering: the collector's
  arena-root cap must survive the revoke — keep it in collector-private
  storage the sweep doesn't scan, or re-derive from the arena base cap
  which is itself exempt.)

Cost: revoke is a kernel sweep (Cornucopia Reloaded made it concurrent
+ per-page). We pay it **once per region per cycle**, amortized over
all objects in the region — not per object.

---

## 3. C-10 — the unified SIGPROT barrier (read + write + assist)

The current `handler.c` (V1, validated) handles the **read** case:
scan cap regs for a tag-0 cap whose address is in `forward_table`,
install the forwarded cap, self-heal the source slot, retry. C-10
extends it to the full concurrent-move barrier:

### 3.1 Decode load vs store

On a SIGPROT (PROT_CHERI_TAG), decode the faulting AArch64/C64
instruction at `ELR`:
- **load** (`LDR Cn,[Cm,...]`): rematerialize the forwarded cap, set the
  destination register, self-heal the source slot, resume. (V1 already
  does the register/self-heal half; add proper instruction decode so we
  set the *load's* destination, not a heuristic reg scan.)
- **store** (`STR Cn,[Cm,...]`): the mutator is writing through a stale
  cap to A. Redirect: compute the forwarded base B, perform the store at
  `B + (faulting_addr - A.base)`, resume past the instruction. This is
  the **write barrier** — and it is the SAME handler, because on Zero
  the store is also a hardware cap-store that traps.

### 3.2 Mutator-assisted evacuation (the race, NS-preserving)

Trap on a revoked A whose object is **not yet copied** (no forward
entry): the mutator must not block on the collector (NS invariant). It
**self-evacuates**:

```
on_trap(A):
  B = forward_table.lookup(A)
  if (B == NULL):                       # not yet moved by collector
     B' = alloc_in(target region, size(A))
     memcpy_caps(B', A, size(A))         # via privileged copy cap
     if (!forward_table.cas_insert(A -> B')):   # lost the race
        B = forward_table.lookup(A)      # collector/another mutator won
        free_unused(B')                  # discard our copy
     else:
        B = B'
  // now forward/redirect using B (load: set reg; store: redo at B)
```

`cas_insert` makes the forward entry the single linearization point:
exactly one copy of A wins (CI invariant — all references see the move
atomically). Whoever loses discards their spare copy. This is ZGC/
Shenandoah self-healing relocation, expressed through the cap-tag trap.

### 3.3 Why reads are simpler than docs/15 implied

A mutator **read** of a not-yet-revoked A sees live A (tag still 1) —
no trap, correct (MI case a). After revoke it traps and forwards (MI
case b). A **write** to not-yet-revoked A lands in A and is captured by
the subsequent copy (because we revoke-first, the only writes that
reach live A happen *before* revoke; after revoke all writes trap). So
the copy never races a concealed write. CI holds.

---

## 4. Forwarding table & revocation engineering

- **Table** (docs/15 §4.1): keep the current flat
  `forward_table` (address-keyed, single-writer-now → multi-writer with
  `cas_insert` for §3.2) for V2; move to the 2-level
  per-region/per-page structure when entry count grows. Entries for a
  region are dropped after it returns to FREE.
- **Revocation epochs**: `cheri_revoke` is epoch-based (we saw
  enq/deq epoch counters in test_basic). A region's forwarding entries
  may be retired only after an epoch boundary guarantees no mutator
  still holds an A-cap. Track per-region "safe-to-drop epoch".
- **`memcpy_caps`**: tag-preserving copy (plain `memcpy` of capability-
  containing memory preserves tags on Morello when done with cap-sized
  aligned copies; verify, else use an explicit cap-by-cap loop).
- **SW_VMEM perm**: object caps must have `CHERI_PERM_SW_VMEM` stripped
  (allocator already does this, allocator.c) or the kernel exempts them
  from the revocation sweep — confirmed necessary in test_basic.

---

## 5. What changes in the code (C-9/C-10 task breakdown)

| area | today | C-9/C-10 |
|---|---|---|
| `stoplessHeap` | single bump arena | region table; alloc from LIVE region; occupancy watermark posts collect request |
| `stoplessArena`/`revoke.c` | whole-arena | per-region revoke; collector privileged copy cap; epoch tracking |
| `StoplessCollectorThread` | — (NEW) | ConcurrentGCThread: mark → select → evacuate loop |
| C-8 root scan | — | `scan_roots(OopClosure)` via Threads/OopStorage/CLDG/Universe; feeds the mark |
| `handler.c` | V1 read self-heal (reg scan) | decode load/store; store redirect; mutator-assisted `cas_insert` evacuation |
| `forward_table.c` | single-writer | `cas_insert` (multi-writer); later 2-level |
| barriers | NOP | stay NOP — the hardware cap-tag IS the barrier (Zero cap-load/store) |

Prerequisite reality check: full end-to-end needs real Java programs
running on Zero (blocked today on the native-call/clone cap bug,
`[[zero-native-call-libffi-blocker]]`). The collector/handler logic can
be unit-tested standalone first (extend `tests/test_basic` into a
concurrent multi-thread mover test, like `tests/test_multi`).

---

## 6. Correctness (MI / CI / NS)

- **MI** (every deref sees old-live or forwarded): pre-revoke → live A
  (tag 1). Post-revoke → trap → handler returns B (forwarded or
  self-evacuated). No third outcome.
- **CI** (move is atomic to all mutators): `forward_table.cas_insert`
  is the single linearization point; the first successful insert fixes
  B for everyone; losers discard spares. Reads and writes after that
  point all resolve to the same B.
- **NS** (neither side blocks the other): mutator never waits on the
  collector — on a not-yet-moved trap it self-evacuates. Collector never
  stops mutators — no STW; revoke runs concurrently (Cornucopia
  Reloaded). Pause time = 0.

---

## 7. Open risks

1. **Trap rate / cost.** A revoke makes a burst of first-touches trap
   (~1-10 µs each: signal + handler). Mitigations: self-heal (done),
   per-thread cap cache (docs/15 §4.2), evacuate small live-sets only.
   This is the main thing C-11's microbench must measure (the ≥3x claim
   is fast-path per-load cost, NOT trap cost — keep them separate).
2. **Instruction decode coverage.** The handler must decode every
   cap-load/store form Zero's clang emits for oop access. Bounded set;
   enumerate from the interpreter's access paths; assert-fail on
   unknown forms during bring-up.
3. **Collector copy cap survives revoke.** The collector needs to read
   A after revoking mutator caps to A. Verify the arena-root-derived
   copy cap is exempt (it is the revocation *target* descriptor, not a
   swept heap cap) — prototype in the standalone multi-thread test.
4. **Revoke granularity vs cost.** Per-region revoke per cycle; if too
   coarse/frequent, batch regions per revoke epoch.
5. **memcpy tag preservation** must be verified for the object sizes
   and alignments HotSpot uses.

---

## 8. Build order

1. C-8 root scan (`scan_roots`) — standalone-testable; also unblocks the
   mark phase.
2. Region table in `stoplessHeap` (alloc + states), no collector yet.
3. `handler.c` V2 (decode + store redirect + `cas_insert` assist),
   validated by an extended `tests/test_multi` (concurrent mover).
4. `StoplessCollectorThread` mark→select→evacuate, single region first.
5. End-to-end on Zero once real programs run
   (`[[zero-native-call-libffi-blocker]]`).
6. C-11 microbench: fast-path per-load cost vs ZGC (the ≥3x claim) +
   pause-time (= 0) + trap-rate characterization.
