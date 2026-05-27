# Caprev runtime debugging status

**Date:** 2026-05-27 (evening)
**File:** `src/cap_runtime/stopless_gc/`
**Status:** Build pipeline solid, 3/4 primitives work, revocation primitive needs more API study.

## What works

- Cross-build from hasee to CheriBSD purecap via morello-sdk + sysroot
- Test binary deploys to QEMU guest, runs as purecap C64 ELF
- `forward_table` (concurrent hash map) compiles + functional
- `handler` (SIGPROT) installs + fires on tag-zero loads
- `stopless_arena_init` mmap's arena, gets shadow via
  `cheri_revoke_get_shadow(SHADOW_NOVMEM, arena, &shadow)`
- `caprev_shadow_nomap_set` returns rc=0 (claimed success)
- `cheri_revoke(IGNORE_START | LAST_PASS)` runs, advances epochs
  (observed: 0→2→4 enq=deq)

## What doesn't (yet)

After `stopless_revoke_now()` returns successfully, the cap to the
marked object **still has its tag set**. Checked at three locations:
- Local register copy
- Re-read from a separately-mmap'd heap slot
- Direct read of slot

All three report `tag=1`.

## Hypotheses

### H1: shadow-bit-to-address mapping wrong

`caprev_shadow_nomap_set_raw` takes `(sbase, sb, heap_start, heap_end)`.
Initial bug was passing `len` as `heap_end` (fixed).

`caprev_shadow_nomap_set` (cap-bounds variant) takes
`(sbase, sb, priv_obj_cap, user_obj_cap)`. Cleaner — uses cap bounds
directly. Confirmed returns 0.

Possible remaining issue: `sbase` should be `cri->base_mem_nomap`
(verified), but maybe the SHADOW returned by `cheri_revoke_get_shadow
(SHADOW_NOVMEM, arena_cap, ...)` is NOT process-global — it might be
specific to the arena_cap's address range. Then `sbase` should be the
arena base, not `base_mem_nomap`.

### H2: kernel sweep skips mmap'd MAP_ANON regions

The cheribsdtest pattern allocates via standard malloc (which lives in
a heap registered with the libmalloc_simple subsystem). Our test uses
plain `mmap(MAP_ANON | MAP_PRIVATE)` for the slot. Maybe kernel only
sweeps registered heaps.

### H3: cap was never actually stored to slot

`*obj_old_slot = obj_old_init` — we verified `tag(slot)=1` BEFORE
revoke. So the cap is there. After revoke, `tag(slot)=1` still. So
the kernel didn't visit this slot during sweep.

### H4: kernel needs explicit MAP_RESERVATION or similar mmap flag

Some CHERI revocation implementations require the user to OPT IN
specific mappings via a `mmap` flag (e.g., MAP_GUARD-like). Worth
checking the cheri_revoke man page or `mmap` flags.

### H5: cheri_revoke not actually doing a full pass

Epochs advance by 2 per call (we see 0→2, 2→4). The +2 might be
"open + close epoch" without actually sweeping. Or the sweep is
empty because no shadow bits actually got set.

## Next-session debug plan

1. **Read shadow bits directly** to confirm marking actually wrote
   non-zero values to the shadow bitmap. (My latest attempt had a
   `#include` inside function body — fix and re-run.)

2. **Try the FULL cheribsdtest revocation flow** as a sanity check —
   if their test program works, replicate exactly.

3. **Read the kernel `sys_cheri_revoke` source** to find what memory
   regions it scans.

4. **Consult Cornucopia Reloaded paper §4** for the exact opt-in
   requirements.

## What this means for the project

The build + scaffold is the high-leverage win — once the revoke API
is figured out (probably 1-2 hours focused work), the remaining
primitives (mover, write barrier, stack snapshot) reuse the same
pipeline. The paper-relevant numbers come from running the mechanism
end-to-end; even if we burn another 2-3 hours on revocation
plumbing, we still hit our 10-week paper timeline (per docs/15 §5).
