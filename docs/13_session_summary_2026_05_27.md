# Session summary 2026-05-27

## Quantified delta

| Metric | Session start | Session end |
|---|---|---|
| Bootclasses loaded (before crash) | 6 | **146** (24× growth) |
| Active patches | 63 | 60 |
| Build cycle | 10-15 min | **75-90s** (10× faster) |
| Documented seams | 0 | 499 categorised |
| Docs | 8 | 13 |

## Commits

```
f215eca  Patch 0069 + scan Category I (255 wordSize sites in aarch64)
3977ac2  docs/12: SIMD codegen wordSize seam analysis
79cd5a6  ⭐ Patch 0068: ObjectAlignmentInBytes auto-bump - 13 -> 146 classes
c63c5ba  docs/11: stack trace recovery + Class mirror allocation analysis
dbd1781  Patch 0067: 9 more metaspace BytesPerWord sites
334edd6  Build infrastructure: fast_iter.sh + scan_cheri_seams.py
efb6d79  Patch 0066: T_ADDRESS injected field (MOJO-aligned)
bf2800d  docs/09: Class injected fields cap-provenance design
b7957ac  ⭐ Phase 1.5 pivot: 16-byte HeapWord (MOJO-aligned)
```

## Biggest wins

1. **Patch 0068 (the gem)** — 26-line change to `set_object_alignment()`
   that bumps `ObjectAlignmentInBytes` from 8 to HeapWordSize under
   CHERI. Without this, `MinObjAlignment = 8/16 = 0` and
   `align_object_size(N)` collapses every instance size to 0. Single
   patch unblocked 11× more bootclasses.

2. **fast_iter.sh** — `gmake hotspot-server-libs` + strip + scp 12MB
   stripped libjvm.so to QEMU. Combined with `procstat -v` for libjvm
   base discovery and core-dump cap-reg parsing for LR walks, it makes
   systematic seam-fixing tractable.

3. **scan_cheri_seams.py** — 9-category static scanner. 499 candidate
   seams enumerated. Discovered patterns we'd never have found via
   per-crash debugging.

## Open work (where the next session would pick up)

### Crash now at f() field overflow (val=0x40 in 6-bit field)

After patches 0066-0068 unlocked 146 classes, the next blocker is a
SIGABRT from `assembler_aarch64.hpp:248`:

```cpp
guarantee(val < (1ULL << nbits), "Field too big for insn");
```

With val=64, msb=15, lsb=10, nbits=6.

Bits [15:10] is the `imms` field in **bitfield-move instructions**
(`sbfm`/`bfm`/`ubfm`). Value 64 trying to encode in 6 bits means
some shift/extract is using shift_amount=64, which is invalid even
for 64-bit data.

Most likely cause: a codegen pattern like `lsl(reg, reg,
log2(SOMETHING))` where `SOMETHING = HeapWordSize` produces a value
that gets fed into bfm-style encoding incorrectly under CHERI.

### __builtin_return_address backtraces unreliable on CHERI

Instrumented the `f()` guarantee to print `__builtin_return_address(0..3)`.
RA[0] resolved into `Assembler::andr` correctly. RA[1..3] gave
ConcurrentHashTable, StringTable, Arena — these are NOT plausible
ancestors of the assembler call chain.

Hypothesis: CHERI's cap stack discipline + LLVM CHERI compiler combo
breaks the unwind-via-frame-pointer pattern that __builtin_return_address
expects.

Worked around once via core-dump LR walks (docs/11). For this
specific f() crash, the call site is inside a deeply-inlined codegen
method which makes the unwind hard. Better debugging would need
explicit cap-aware stack walker (e.g., libgcc_eh _Unwind_Backtrace
might work; not tried).

### wordSize misuse audit (Category I, 255 hits)

Three patches (0059, 0069, prior 0058) addressed ~10 sites. ~245
remain. Categorise per docs/12:
- **machine word size** (intended): keep wordSize
- **Java stack slot size**: replace with BytesPerLong
- **capability size**: new constant BytesPerCap

Many are clearly fine (machine-word semantics under CHERI ARE
cap-sized). Others need code reading. fast_iter makes the iteration
loop ~80s so this is tractable but tedious.

## Key conceptual breakthroughs

1. **MOJO's T_ADDRESS is the correct mechanism for cap-typed Java
   fields.** Patch 0066 reproduces it exactly. This validates our
   approach against published prior work.

2. **CHERI's `wordSize` is sizeof(intptr_t), not "machine word".**
   This drives most of the alignment bugs. Renaming a single
   constant (wordSize → MachineWord vs JavaSlot) would have prevented
   ~50 bugs.

3. **The `8/16=0` integer-division class of bugs is THE bug pattern
   on CHERI.** When `HeapWordSize`/`MinObjAlignment`/`HeapWordsPerLong`
   are computed via integer division and the divisor exceeds the
   dividend, you silently get 0 instead of an error. align_up(N, 0)
   = 0 makes data structures collapse.

## Reusable techniques (worth keeping)

- `scripts/fast_iter.sh` for 80s edit→build→deploy→test cycle
- `scripts/scan_cheri_seams.py` for systematic seam discovery
- `procstat -v PID` to find libjvm.so runtime base via slept-then-snapshot trick
- Stack-frame chain walk via SP+0/SP+0x10 in `dd skip=... | od`
- `__builtin_return_address(0)` works at one level on CHERI; higher
  levels unreliable
- Cap-register dump from FreeBSD core via NT_PRSTATUS section parsing
  (see docs/11)
