# C-6 next-session resume — SIGPROT pinpointed in CodeCache PC

**Date:** 2026-05-28 (end of session, continuation of docs/24)
**Last commit:** `dcb0980` + this doc.

Picks up from docs/24's "next blocker is String.<clinit>". After
installing the cap_runtime SIGPROT handler early (patch 0087), the
crash is no longer silent — we have a precise faulting PC.

## The exact crash

```
[Stopless] SIGPROT handler installed                          ← installed
...
[2.976s] Initializing 'java/lang/String' (0x...)              ← Java starts
[stopless] unforwarded fault: si_addr=0x428b8138 si_code=2 \
           pc=0x428b8138 fwd_size=0 \
           (no tag-zero cap in regs matched)
exit=162  (= 128 + SIGPROT)
```

Decoded:
- `si_code=2` = `PROT_CHERI_TAG` — capability tag violation.
- `pc == si_addr == 0x428b8138` — fault came from PC fetch, not a data
  load. CPU tried to execute through a cap whose tag bit is 0.
- `fwd_size=0` — forwarding table is empty (no GC moves have run).
- "no tag-zero cap in regs matched" — none of the 30 cap registers
  held a tag-zero cap. Consistent with the fault being on PC itself,
  not on a load-via-cap.

## Where is PC 0x428b8138?

**NOT in `libjvm.so .text`.** libjvm.so .text on this build ranges
roughly 0x40000000–0x40E00000. The PC is ~0x428xxxxx, past .text.

Most likely in the **HotSpot CodeCache** (where interpreter, stubs,
and JIT'd methods live). The CodeCache is a separate mmap'd region
above libjvm.so.

## Why is the PC cap tag-zero?

Hypotheses, in rough order of likelihood:

1. **A function pointer / return address was stored to a 4-byte
   narrow slot somewhere and lost its tag.** Same family as the
   bug-4 CompressedOops issue, but for code caps. Even with
   `-XX:-UseCompressedOops`, some specific path may still use a
   non-cap-sized slot for a code pointer.
2. **Branch/dispatch table emitted by interpreter generation uses
   non-cap relocations.** The interpreter dispatch table holds
   ~256 entry-point addresses (one per bytecode). If those are
   stored as 8-byte addresses instead of 16-byte caps, the BR/BLR
   to a table entry loads a tag-zero "address" into PC.
3. **An indirect call target read from a klass method table where
   the slot type was wrong.** Klass tables hold Method* pointers
   (caps on purecap). If something parses them as 8-byte pointers,
   subsequent calls land on tag-zero.

The signature ("no tag-zero cap in regs" + "fault PC == si_addr")
fits all three. Likeliest is (2) because String.<clinit> is the
first method execution, and the interpreter is the path most freshly
exercised here.

## Tools needed to make further progress

The diagnostic gap is now: we have PC, but no easy mapping from
PC to source. The libjvm.so `addr2line` we used in docs/24 won't
resolve PCs in CodeCache.

Options for next iter:

### Option A — use HotSpot's own PrintInterpreter / PrintStubCode
Add `-XX:+PrintInterpreter -XX:+PrintStubRoutines -Xlog:codecache=info`
to the next run. PrintInterpreter dumps the dispatch table with
disassembly + entry-point addresses. If 0x428b8138 falls inside an
entry, we know exactly which bytecode handler.

Caveat: PrintInterpreter may itself crash (Java code execution path);
need to print BEFORE String.<clinit> runs. Maybe use
`-XX:+CompileTheWorld=0` or similar startup-stop-after-stubs flag.

### Option B — attach gdb to the QEMU-guest java process
1. Add `-XX:-CreateCoredumpOnCrash` and  `-XX:+ShowMessageBoxOnError`
   so the JVM stops at the crash and waits.
2. From a second SSH session into QEMU, attach `gdb -p $(pgrep java)`.
3. `bt` to get the stack including CodeCache frames.

CHERI-aware gdb may or may not handle this gracefully. Worth a shot.

### Option C — instrument templateInterpreter_aarch64.cpp generation
Print each generated entry point's address as it's emitted. Then
the runtime PC can be mapped to bytecode name from the printout
without needing -XX:+PrintInterpreter.

Recommend C: deterministic, doesn't depend on Java-side running.

## Commits today (chronological)

```
dbd2577  Phase C-3 + C-4: StoplessArena C++ wrapper + bump-pointer allocate landed
f86acc5  docs/23: C-6 resume snapshot — post-Genesis crash, GC-agnostic
dcb0980  Phase C-6 progress: 4 root causes fixed, JVM reaches first Java method
+ this docs/25 + patch 0087
```

## Repro (still requires both compressed flags)

```bash
ssh -p 10005 root@localhost \
  '/opt/jdk/bin/java -XX:+UnlockExperimentalVMOptions -XX:+UseStoplessGC \
     -Xms16m -Xmx32m \
     -XX:-UseCompressedClassPointers -XX:-UseCompressedOops \
     -Xlog:class+init=info \
     -version'
```

Expected output (with patch 0087 applied):
```
[Stopless] SIGPROT handler installed
... (init progresses to)
[N.NNNs] Initializing 'java/lang/String'
[stopless] unforwarded fault: pc=0x428b8138 ...
exit=162
```
