# gdb-cheri attach success — fault is CHERI BOUNDS, not SIGILL

**Date:** 2026-05-28 (deep evening, continuation of docs/29)
**Method:** Installed `gdb-cheri-14.1` from `pkg64` (hybrid-ABI repo)
inside CheriBSD QEMU guest. Ran java under gdb with `catch signal
SIGILL` (which actually triggered on a different signal, see below).

## The diagnostic that finally pinpointed it

```bash
ssh root@QEMU_GUEST '/usr/local64/bin/gdb -batch \
  -ex "catch signal SIGILL" \
  -ex "run -XX:+UseStoplessGC ... -version" \
  -ex "info registers pc x0 x12" \
  -ex "info registers c0 c12" \
  -ex "x/8i \$pc-16" \
  -ex "info symbol \$pc" -ex "bt 10" \
  --args /opt/jdk/bin/java ...'
```

Result:

```
Thread 2 received signal SIGBUS, Bus error.
Invalid address alignment.
0x00000000428bc2c0 in ?? ()

pc             0x428bc2c1
x0             0x42681440
c0             0x42681440 [rwRW,0x42681440-0x426814c0]    ← BOUNDS = 128 BYTES
c12            0x4b045ae0 [rwxRWE,0x4abb8000-0x4f3b8000]

0x428bc2c0:	ldp	c9, c16, [c0, #-224]!
0x428bc2c4..0x428bc2cd:	.inst ... ; undefined  (garbage / padding)

#0  0x00000000428bc2c0 in ?? ()
#1  0x00000000428b41b0 in ?? ()      ← inside call_stub (was at 0x428b4138 start)
```

## What this means

**The fault is not SIGILL** — it's `SIGBUS, Invalid address alignment`,
which on CheriBSD wraps a `PROT_CHERI_BOUNDS` violation. The kernel
reports CHERI bounds faults as SIGBUS, which is why our SIGILL handler
in `cap_runtime/stopless_gc/handler.c` never fired.

**The instruction at PC** is `ldp c9, c16, [c0, #-224]!` (cap-LDP
pre-index, decrement c0 by 224 bytes then load 32-byte cap pair).

**The fault root**: c0 (= `c_rarg0`, holding `JavaCallWrapper* &link`
from JavaCalls::call_helper) has CHERI bounds tight to the wrapper
struct, [0x42681440, 0x426814c0) = 128 bytes. Pre-decrement c0 by
224 makes the new address (0x42681360) **fall below c0.base**, which
is a CHERI bounds violation.

## Why the instruction looks "right" in encoding but "wrong" in semantics

The encoding is a valid Morello cap-LDP pre-index. The bug is that
**c0 wasn't the right base register**. The HotSpot interpreter's
`normal_entry` prologue saves callee-saved registers to a -224 stack
slot; on plain AArch64 it uses SP as base, but on CHERI we need
**csp**. With the standard `Assembler::stp/ldp` emitting integer
encodings, that came out as `stp x_, x_, [sp, ...]`. Some later cap-port
attempt (still being investigated) converted it to a cap-LDP but kept
c0 as base — likely a HotSpot-side register-alias confusion where
`r0` and `c_rarg0` collide.

## Source-side detective work (TODO next session)

Search for the emit site of `ldp c_,c_, [Rn, #-224]!` with Rn=0:
* `templateInterpreterGenerator_aarch64.cpp` line 805 has
  `__ stp(esp, zr, Address(__ pre(sp, -14 * wordSize)))` — STP not
  LDP, base = sp not r0. Possibly the matching LDP in a different
  generator routine has the bug.
* `generate_fixed_frame()` at line 1564 of generate_normal_entry —
  one of the prologue helpers — is a strong candidate.

Most likely culprit:
* Some HotSpot common-frame helper (shared, not aarch64-specific) calls
  `__ ldp(rN, rN+1, Address(__ pre(rfp, ...)))` with a register that's
  defined as `r0` on AArch64 but is actually a stack-pointer alias on
  most platforms. The platform-mapping table for HotSpot register
  abstractions (in register_aarch64.hpp) may have an entry that aliases
  `r0` to `rfp` incorrectly under CHERI.

## Session totals after gdb breakthrough

* 18 commits this session
* 59 build iterations + 1 successful gdb-cheri-attached run
* **17 bugs identified**: 16 with patches landed, the 17th (this LDP
  pre-index) now pinpointed to specific instruction + register state.
* Full empirical Morello cap-ISA encoding reference (docs/27)
* `gdb-cheri` is now installed in the QEMU guest and can be used
  for the same diagnostic on next session's builds with one command.

## Next session — the 2-iteration path to crossing String.<clinit>

1. Grep HotSpot for `__ ldp(.*,.*pre.*r0` and `__ ldp(.*,.*pre.*rmethod`
   patterns, identify the specific source line.
2. Replace the offending base register with `csp` (or equivalent),
   OR widen `c_rarg0`'s bounds before BLR-ing into normal_entry (give
   it a "stack capability" cap, not a tight wrapper cap).
3. Build, ship, rerun under gdb. If SIGBUS still fires at a NEW pc,
   repeat. Otherwise we're past String.<clinit>'s first bytecode and
   can profile further with `-Xlog:class+init=info`.

Estimated 3-5 iterations to reach `java -version` exit 0.

## How to repeat the gdb diagnostic from scratch

(For any future bug in the same family.)

```bash
# 1. Ensure gdb-cheri is in the QEMU guest:
ssh root@guest 'ASSUME_ALWAYS_YES=yes pkg64 install -y gdb'

# 2. Run java under gdb with signal catches:
ssh root@guest '/usr/local64/bin/gdb -batch \
  -ex "set confirm off" -ex "set pagination off" \
  -ex "catch signal SIGILL" -ex "catch signal SIGBUS" -ex "catch signal SIGSEGV" \
  -ex "run -XX:+UseStoplessGC ..." \
  -ex "echo \\n=== CAUGHT ===\\n" \
  -ex "info registers pc x0 x12 x29 x30" \
  -ex "info registers c0 c12 c29 c30 csp" \
  -ex "x/8i \$pc-16" \
  -ex "bt 10" \
  --args /opt/jdk/bin/java ...'
```

(`gdb-cheri` decodes capability registers, including bounds, perms,
and the C64 mode bit — the key diagnostic that fprintf alone could
never provide.)

## What this opens up for paper §3

The diagnostic infrastructure used for the C-6 cascade — cap_runtime
SIGPROT handler with register-state dump, then build-time byte
verification, then `gdb-cheri` attach as final-fallback debugger — is
itself a research artifact. A "Porting Experience" section for the
paper should cover:

1. Why standard signals (SIGSEGV, SIGILL) are insufficient on CHERI
   (PROT_CHERI_* faults wrap as SIGBUS / SIGSEGV with non-standard
   si_code).
2. Why HotSpot's own crash handler is capability-unsafe in early init
   (we observed it recurse on bad caps and hang).
3. Why `gdb-cheri` is essential: it shows cap bounds + perms + the
   C64 mode bit, none of which standard gdb reports.
