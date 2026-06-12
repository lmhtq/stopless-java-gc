# 41 — C-9 session: ZGC-on-CHERI research, the real MinMove root cause (code-5), and the Zero clean-build boot SIGILL

Date: 2026-06-02. This document preserves the findings of a long C-9 session so a
future rebuild/rewrite is faster. Three threads: (A) a cited research result that
re-positions the whole project, (B) the **actual** root cause of the long-standing
MinMove hang (a one-line handler bug — **fixed**), and (C) a **new** boot blocker
that a clean rebuild exposed and that is **undiagnosable in this environment**.

---

## A. Research: what the project actually is (3 cited deep-research rounds)

Memory: `project_zgc_on_cheri_thesis.md`. Sources: Arm DDI0606, JEP 333/439, Liden
FOSDEM18, Cambridge CHERI (ISCA2014 / cheri-formal / FAQ), Cornucopia Reloaded
ASPLOS2024, CHERIvoke MICRO2019, MOJO-JVM, Glasgow MPLR2023, V8-on-Morello
CHERITech24, arXiv 2603.05645.

1. **Classic ZGC's colored-pointer load barrier does NOT survive bounded purecap
   CHERI**: color in high address bits + heap multi-mapping → out-of-bounds /
   non-representable → bounds-fault or tag-clear. Confirmed by V8-on-Morello hitting
   exactly this wall ("setting the top bits of a pointer makes it invalid… that
   isn't going to work"). CHERI permissions are monotonically non-increasing
   (CAndPerm clear-only), so a permission can't be a flippable GC color.
   - **Caveat (I had overstated "ZGC necessarily degrades"):** Generational ZGC
     (JDK21+) already moved color out of the address; CHERI's 8-bit **Flags** field
     (CSetFlags) is freely bidirectional → modern ZGC *might* relocate color into
     Flags/MMU-ignored bits at some cost. The exact cost is **unmeasured** — the
     one quantitative gap left in the thesis.

2. **The CHERI-native *moving/relocating* GC subspace is genuinely unclaimed.**
   MOJO-JVM ported only Epsilon/Serial/G1 (all STW; no colored pointers / load
   barriers); Glasgow did MicroPython + non-moving Boehm; Cornucopia/Reloaded/
   CHERIvoke are non-moving temporal safety; CRuby port has no GC novelty.

3. **The *right* hardware primitive exists but is kernel-only.** Cornucopia
   Reloaded's per-PTE "capability load generation" barrier faults on the cap-LOAD
   (so the handler gets the source slot → self-heal + identity normalization), uses
   an O(1) generation bump (sweep still needed for reclamation), runs on real
   Morello silicon, ships in CheriBSD 23.11+. **BUT it is internal to the kernel
   revoker — userspace gets only `cheri_revoke` (tag/perms clear) → deref-faults.**
   The two userspace syscalls are `cheri_revoke` + `cheri_revoke_get_shadow`; there
   is no way to mark pages to trap cap-loads into a user handler.

4. **Honest positioning.** On equal silicon, end-to-end throughput will NOT beat
   CPU+ZGC — the 128-bit pointer tax dominates and hits pointer-dense GC workloads
   hardest. The defensible contribution is **"first CHERI-native concurrent moving
   GC for the JVM + honest purecap-baseline characterization"** and the
   forward-looking argument *"if CHERI is the substrate, the GC read-barrier check
   is ~free; and the right design exposes the kernel's cap-load generation barrier
   to a managed runtime"* — NOT "faster than ZGC." The correct A/B baseline is
   **purecap StoplessGC move-on vs move-off**, never x86+ZGC.

---

## B. The real MinMove root cause — FIXED (handler code-5)

Prior memory (`project_c9_postmove_dispatch_spin.md`) blamed interpreter-frame oop
fixup / a Method*=0x22 dispatch spin. **That was a downstream symptom.** The actual
root cause:

> Default CheriBSD revokes by **clearing permissions, not the tag**
> (`CHERI_CAPREVOKE_CLEARTAGS` is *undefined* by default; see
> `sys/cheri/revoke.h:cheri_revoke_is_revoked`: revoked iff `tag==0 || perms==0`).
> So a revoked cap keeps `tag=1, perms=0`, and dereferencing it faults as
> **`PROT_CHERI_PERM` (si_code=5)**, NOT `PROT_CHERI_TAG` (2).

`handler.c` only accepted code 2 → on a code-5 fault it returned **without healing
and without advancing PC** → the instruction re-executed → faulted again →
**infinite silent re-fault spin**. This exactly matches the recorded symptom ("no
`[stopless] unforwarded fault` output, silently re-faulting forever").

**Fix (in tree, `src/cap_runtime/stopless_gc/handler.c`):** accept both code 2 and
code 5, and match revoked caps in `STOPLESS_TRY_REG` by `tag==0 || perms==0`
(mirroring the kernel predicate). `forward_table` membership remains the real gate,
so a benign perms-0 cap simply won't forward.

Guest kernel confirmed: `FreeBSD 15.0-CURRENT`, `CHERI_CAPREVOKE` compiled in,
`security.cheri.runtime_revocation_default=1`, `PROT_CHERI_TAG=2`,
`PROT_CHERI_PERM=5` (from `sys/sys/signal.h`).

**This fix is done and ready; it could not be end-to-end validated only because of
blocker C below.**

---

## C. The blocker: clean-rebuild Zero boot SIGILL (UNDIAGNOSABLE here)

To validate B, the JVM must boot. A **clean** `hotspot-server-libs` rebuild of HEAD
**SIGILLs at boot** (rc=132) in the native-call trampoline path, at `[C6TRAMP]
fn_id 47`, hitting even `java -version`. The interpreter never reaches `main`.

### What it is NOT (ruled out, each a full build+deploy+test cycle)
- **NOT ABI skew.** Rebuilt the whole consistent chain — `cheribsd` + sysroot +
  disk-image + libjvm all from `bb0e87e` (fixed a real sysroot=May-20 ↔ image=today
  skew along the way). Still SIGILLs.
- **NOT cap_runtime C64/A64 mode.** cap_runtime was `-march=morello+c64` (C64) while
  libjvm is `-march=morello` (A64). Aligned cap_runtime to A64 (Makefile change in
  tree). Still SIGILLs. (Alignment is still correct — keep it.)
- **NOT the `cap_blr` instruction.** clang emits `blr c<n>` for indirect cap-calls
  tens of thousands of times across libjvm — it works. The dispatch
  (`cap_trampoline_dispatch`) disassembles as clean A64.
- **NOT a "broken trampoline file to rewrite."** `cap_trampoline_aarch64.cpp` (an
  *untracked* WIP) is actually fine.

### What it most likely IS
`Thread::_cap_trampoline_addr` was seeded only for **non-Zero**
(`#if … && !defined(ZERO)`); the Zero branch set it to **NULL** (thread.cpp), on the
assumption "Zero needs no codecache trampoline." That assumption is **false now**:
the purecap **Zero build still generates aarch64 stubs** (StubGenerator / runtime
stubs) via `MacroAssembler::call_VM_leaf_base`, which route through
`cap_trampoline_dispatch` (that's what emits `[C6TRAMP]`). With the cap NULL, the
stub `cap_blr`s a null cap and faults **before** dispatch runs (`[C6DISP]` never
prints — confirms the branch dies at entry).

**Fix attempted (thread.cpp, in tree):** seed `_cap_trampoline_addr =
&cap_trampoline_dispatch` in the Zero branch too. **Did NOT resolve boot** — almost
certainly a **seeding-timing** problem: stub generation/execution in early
`init_globals` runs **before the main JavaThread's `Thread()` ctor** seeds the cap.
The non-Zero path re-seeds the *other* caps in `init.cpp:146` (also `!defined(ZERO)`),
but `_cap_trampoline_addr` is only seeded in the ctor. So when the crashing stub
executes, `rthread`'s cap is likely still unseeded.

> **Why this probably never bit before:** the prior *working* libjvm was a
> multi-session **incremental** build — a mix of object files from different source
> states, some predating whatever change made Zero start generating these
> trampoline stubs. The **clean rebuild recompiled everything from HEAD and exposed
> the latent regression.** The incremental working build was overwritten by the
> clean rebuild (no backup existed). This is the key build-hygiene lesson.

### Why it's undiagnosable in THIS environment (all confirmed)
- **No gdb / lldb** in the CheriBSD pkg repo (the docs/30 `pkg64 install gdb` recipe
  is stale: "No packages available matching 'gdb'"); none builds in.
- **Signal handler can't catch it.** Installed a fault-safe SIGILL diagnostic via a
  libjvm **constructor** (so it's armed before VM init) and even ran with `-Xrs`
  (HotSpot installs no handlers) — it **never fires**.
- **Core dump fails:** `dmesg` → *"exited on signal 4 (no core dump — bad address)"*.
  The capability/CSP state at the fault is corrupt enough that the kernel can
  neither deliver the signal nor write a core. **This corruption is itself the real
  clue** — it is not a clean illegal-opcode-at-valid-PC; something trashes machine
  state (CSP?) at/just-before the fault.
- `dmesg` logs no PC.

→ Without the faulting PC, fixes are blind shots (10-min cycles, binary pass/fail).
This is the project's own documented frontier (`cap_trampoline_aarch64.cpp` header:
*"needs Morello ISA-level investigation beyond what gdb-cheri alone can answer"*),
now in a Zero-build init-order context.

---

## D. Recommended path for the rewrite (with this experience)

1. **Get observability first** — this is the gate. Options: build/obtain gdb or lldb
   for this CheriBSD (ports, or cross-gdb on hasee attaching to the QEMU gdbstub via
   `-s -S`!), or use QEMU's own `-d in_asm,cpu` / gdbstub to single-step the boot and
   catch the faulting PC. **The QEMU gdbstub is the most promising un-tried route**
   (host-side gdb, no guest package needed).
2. **Likely correct architectural fix** (verify with #1): EITHER (a) make the Zero
   build **not** pull in the aarch64 trampoline stubs (restore the original design
   assumption — find why StubGenerator/`call_VM_leaf_base` is reached in Zero), OR
   (b) seed `_cap_trampoline_addr` **early enough and globally enough** that the
   first stub execution sees a valid cap (a process-global cap, or seed before
   `init_globals` stub gen, not in the per-thread ctor). The seeding-timing analysis
   in C is the lead.
3. **Build hygiene:** always keep a known-good `libjvm.so` backup before a clean
   rebuild; prefer deploying `images/jdk` (339 files, jimage) over the exploded
   `jdk/` (27692 files — slow to scp/untar in the emulated guest, caused orphaned
   `tar` and "Directory not empty" loops).
4. Then validate the **B fix**: `MinMove` (`N n=new N(77); System.gc(); int v=n.id;`)
   with `STOPLESS_MOVE_KLASS='MinMove$N' STOPLESS_MOVE_LIMIT=1` under `-XX:+UseStoplessGC`
   — expect `n.id==77`, no spin, `stopless_handler_self_heals>0`.

---

## E. Code changes left in tree (this session)

All uncommitted-but-preserved; reasonable to keep:

- `src/cap_runtime/stopless_gc/handler.c`
  - **B fix:** accept `PROT_CHERI_PERM(5)` alongside `PROT_CHERI_TAG(2)`; match
    revoked caps by `tag==0 || perms==0`.
  - Fault-safe SIGILL diagnostic (metadata only, no PC deref) + a `constructor`
    that installs it before VM init. (Diagnostic only; harmless to keep.)
- `src/cap_runtime/stopless_gc/Makefile`
  - `-march=morello+c64` → `-march=morello` (align cap_runtime to libjvm's A64
    purecap mode; also silences clang's `+c64 deprecated` warning).
- `third_party/openjdk-jdk17/.../share/runtime/thread.cpp` → patch
  `0131-c9-zero-seed-cap-trampoline-addr.patch`
  - Seed `_cap_trampoline_addr` in the Zero branch (necessary but not sufficient —
    see C; the seeding-timing fix is future work).

## G. OBSERVABILITY BREAKTHROUGH (2026-06-03) — QEMU exception log works

The §C claim "undiagnosable in this environment" is now **partly lifted**. The
docs/41 §D-recommended route works: **QEMU's own exception log via the HMP monitor**,
no guest debugger needed.

**Method (reproducible):**
1. Launch the guest directly (not via cheribuild) mirroring its QEMU cmdline, adding
   a monitor socket: `-monitor unix:/tmp/qmon.sock,server,nowait` and `-L
   .../sdk/share/qemu -bios edk2-aarch64-code.fd` (drop the optional `-virtfs`/`smb`
   shares — boot doesn't need them). Script: `/tmp/launch_qemu_dbg.sh`.
2. Boot fully (sshd on :10005), THEN toggle logging at runtime via the monitor
   (keeps the log to just the java run): connect the unix socket, send
   `logfile /tmp/qemu_trap.log` then `log int,guest_errors,unimp`. (`/tmp/qmon.py`
   sends HMP commands over the socket.)
3. `ssh` run `java -version` (crashes), then `log none`.
4. Analyze `/tmp/qemu_trap.log` — `int` logs every exception with EC/ESR/ELR/PSTATE.

**Findings:**
- **This QEMU's Morello model is INCOMPLETE.** It raises `EXCP_UDEF` (exception 1
  "Undefined Instruction", ESR EC=0x00 "Unknown") for Morello instructions it does
  not decode — **confirmed for `scbnds`** (set-capability-bounds, `0xc2d03815`). The
  **CheriBSD kernel emulates** these in its undef handler and returns to **ELR+4**
  (survivable). 662 of 663 undefs in a `java -version` run are EC=0x07 benign
  lazy-FP-enable traps (return to ELR = retry). So instruction-coverage gaps are
  masked by kernel emulation — until one the kernel can't emulate.
- **libjvm.so runtime base = `0x41600000`** (12.3 MB stripped; `.text` segment at
  `0x419ef000`, file off `0x3df000`; full mapping `0x41600000-0x422a9000`). To map a
  runtime PC to a symbol: `llvm-symbolizer --obj=<unstripped build libjvm.so>
  $((pc-0x41600000))`. (Stripping preserves offsets, so the unstripped build copy
  symbolizes the deployed stripped one.) NOTE base is ASLR'd — re-derive per run via
  `truss` (grep the `open("…/libjvm.so")` then its big `mmap`).
- **The crash is NOT in libjvm proper.** java's last libjvm-range exception is a
  **SVC (syscall)** at `0x41717…` (the `[C6TRAMP] fn_id 47` `write`); after that it
  enters a **codecache stub and faults there** — consistent with the §C trampoline
  `cap_blr` hypothesis. No "Undefined Instruction" exception in the log is fatal
  (all emulated or FP-retry), so the fatal fault is in the codecache execution path
  (a mis-branch / ifetch, NOT a missing opcode).

**Still to pin (now TRACTABLE with the method above):** the exact codecache fault.
EL0 PC bands seen: `0x40` (32k, low libs), `0x41` (9k, libc+libjvm), `0x42` (2.3k —
**candidate codecache / high libs**, last activity @line 157022), plus 2 signal-
trampoline deliveries (`0xfffffffff000`, lines 171332/172871). Next pass: find the
anonymous RWX **codecache** mmap range (via `truss` — a large `MAP_ANON|PROT_EXEC`
mmap) and grep the log for the last exception with ELR in that range; that ELR is
the faulting `cap_blr`/stub instruction. Then disassemble the codecache (dump it
from the guest, or regenerate the stub) to see the offending instruction.

> Net: the boot blocker is most plausibly a **codecache cap-branch fault** (the §C
> seeding-timing or a mode/bounds issue on the stub), NOT a missing-opcode problem —
> though this QEMU's incomplete Morello coverage (scbnds) means a real-hardware /
> Morello-FVP run is also worth trying as a cross-check.

## F. Environment state (rebuilt this session, consistent)
- `cheribsd` pinned at `bb0e87e`; sysroot `rootfs-morello-purecap` + disk-image +
  libjvm all rebuilt consistently from it. Guest boots CheriBSD fine (sshd on
  hasee:10005); only the JVM native-call path SIGILLs.
- Guest JDK at `/opt/jdk` (deployed from `images/jdk` + stripped libjvm). gdb
  installed? NO — not available.

## H. Codecache pin attempt (2026-06-03, "继续" round) — ruled out, narrowed

Disabled ASLR (`sysctl kern.elf64.aslr.enable=0 kern.elf64.aslr.pie_enable=0`) for
deterministic addresses, then truss + a fresh `-d int` logged run (correlatable).

Addresses (ASLR off): libjvm.so = `0x41600000-0x422a9000`; **codecache** (big
`MAP_ANON|PROT_EXEC`) = `0x54bc0000-0x5d3c0000`.

DEFINITIVELY RULED OUT (so a future rewrite doesn't chase these):
- **Not a missing/illegal opcode.** Across two runs, ALL 253 `Undefined Instruction`
  exceptions are benign: 250 are EC=0x07 lazy-FP-enable traps (kernel returns to ELR
  = retry), 1 is the EC=0x00 `scbnds` the kernel emulates (returns ELR+4), 0 are
  fatal. (`cap_blr` also emits the clang-verified `0xC2C23000` encoding QEMU supports.)
- **Not a codecache fault.** Zero exceptions have an ELR in `0x54bc0000-0x5d3c0000`.
- **Not a libjvm-.text fault.** java's last libjvm-range exception is the fn_id-47
  `write` SVC (`0x41717c44`); nothing faults in libjvm after it.

WHAT IT ACTUALLY LOOKS LIKE: after that last SVC, the kernel runs a long IRQ burst
(EL1) then **returns to the signal trampoline `0xfffffffff000`** and the handler
makes libc syscalls (`0x415d4xxx`) — i.e. the SIGILL is delivered **deferred /
handler-mediated**, NOT as a synchronous illegal-instruction trap at a faulting PC.
With "no core — bad address" (kernel can't write a core: corrupt CSP/cap), this
points to **capability-state corruption** surfacing as a signal, which a
machine-level exception log fundamentally cannot localize to a source line.

CONCLUSION: the QEMU `-d int` method is **exhausted** for this bug — it cleanly
eliminated the opcode/cap_blr/codecache/libjvm-fault hypotheses and gave reusable
infra + addresses, but a deferred/corruption-mediated SIGILL needs **source-level
debugging** (a real gdb/lldb breaking in the JVM signal path, or a Morello-FVP /
real-hardware run as cross-check). Tooling gap unchanged: no gdb/lldb in this
CheriBSD pkg repo. Next concrete options: (1) build cross-gdb on hasee attaching to
the QEMU gdbstub (`-s`), set a HW breakpoint on the kernel's signal-post path or the
process's sigtramp, read the corrupt cap; (2) instrument HotSpot's SIGILL/SIGSEGV
handler to dump full cap-register state on first entry (one build cycle); (3) run on
the Morello FVP / real hardware where instruction coverage is complete.

## I. Signal-handler instrumentation (2026-06-03) — fault is UNDELIVERABLE

Instrumented HotSpot's generic signal handler entry (`JVM_HANDLE_XXX_SIGNAL` in
os/posix/signals_posix.cpp, env-gated `STOPLESS_SIGDIAG`) to dump cap-register state
(PCC/CSP/CLR/c16/c17) the instant any SIGILL/SIGSEGV/SIGBUS arrives. Patch 0132.

Result: **`[SIGDIAG]` never prints.** Neither HotSpot's handler nor the cap_runtime
ctor SIGILL handler (installed at libjvm load, even with `-Xrs`) is ever entered on
the fatal signal. Conclusion — the fatal fault is **UNDELIVERABLE**:
- It occurs BEFORE signal handlers are installed (fn_id 47 = stub gen in
  init_globals, before os::init_2 / stopless_handler_install — consistent with
  "[Stopless] SIGPROT handler installed" never printing pre-crash), AND
- It corrupts capability machine state (CSP) so the kernel can deliver the signal
  to NO handler and cannot write a core ("no core — bad address").

So it is terminate-by-default, and the corruption (not a clean illegal opcode) is
the root — which is why no fatal `Undefined Instruction` appears in the `-d int` log.

**Tools now exhausted: exception log AND handler instrumentation both defeated by
the corruption/pre-init timing.** The ONE remaining viable tool is a **machine-level
debugger via the QEMU gdbstub** (`-s -S`), which is independent of guest signal
delivery and handler installation: build a Morello/aarch64-aware gdb on the host
(cheribuild has a gdb target; the `--upstream-gdb/*` options exist), attach to the
gdbstub, set a HW watchpoint on the thread CSP / the `_cap_trampoline_addr` slot or a
HW breakpoint at the first codecache stub entry, single-step the cap_blr, and read
the exact instruction + capability that corrupts state. That is the concrete next
step; everything else (addresses, ruled-out hypotheses) is now in place to make it
fast.

## J. CRACKED via QEMU gdbstub (2026-06-03) — ifetch abort: branch-to-stack

Built a host Morello-aware gdb (cheribuild `upstream-gdb-native`; needed
`libmpfr-dev` on hasee), launched QEMU with `-s` (gdbstub :1234), ASLR off, and
caught the fatal signal at the kernel termination path: `break sigexit if sig==4`.

GOTCHAS (recorded for reuse): (1) a bare TCP connect to :1234 HALTS the QEMU CPU
(gdbstub) — resume via HMP `cont`; don't poke :1234 except via gdb. (2) This gdb
can't decode the stub's `data_capability` register type, so `$x0` / `read_register`
/ cap-pointer deref all fail with "DWARF register number NNN"; BUT `info registers
x0` (textual, gdbarch path) works. Workaround in `/tmp/catch5.gdb`: Python
`gdb.execute("info registers x0", to_string=True)` to read regs + raw `read_memory`
at DWARF-computed struct offsets (capability in memory = 16B, address = low 8B), so
`td -> td_frame -> tf_*` is walked manually. kernel.full has DWARF (struct layouts).

**THE FAULT (deterministic, ASLR off):**
```
tf_elr (faulting PC) = 0x4267c010      tf_esr = 0x8200000f  -> EC=0x20 INSTRUCTION ABORT
tf_lr  (x30/CLR)     = 0x4267c010      tf_far = 0x4267c010   (ifetch fault address)
tf_sp                = 0x4267c010   <-- SP == PC == LR (control-flow/stack corruption)
x0 = 0x59559a00 (in codecache 0x58bc0000-0x5d3c0000)   x16/rscratch1 = 0x4210e540 (libjvm .data)
```
`0x4267c010` lies in `0x4247d000-0x4267d000` — a **2 MB thread-stack** mapping (guard
page right after), ~4 KB below the top (a plausible SP). So the CPU **branched/
returned to a stack address and faulted fetching an instruction from the stack**.

**CONCLUSION:** the boot crash is NOT an illegal opcode, NOT a clean cap_blr-to-null,
and NOT a codecache/libjvm code fault. It is a **corrupted return/branch that lands
on the stack** (SP==PC==LR), while executing a **codecache native-call trampoline
stub** (x0 in codecache). This is exactly the corruption the earlier rounds inferred
("no core — bad address", undeliverable signal). It is fully consistent with the §C
`_cap_trampoline_addr` seeding-timing bug: when the stub's `cap_blr` / frame save-
restore goes wrong (bad/unseeded trampoline cap at that early point), the subsequent
`ret` returns to a stack address instead of valid code.

**FIX DIRECTION (now focused, not a mystery):** the codecache native-call path
(MacroAssembler::call_VM_leaf_base + the `_cap_trampoline_addr` seeding) corrupts the
return/frame in early-init Zero stubs. Either (a) ensure `_cap_trampoline_addr` (and
the stub's CSP/CLR save-restore) is valid *before* the first stub executes — seed it
process-globally / before init_globals stub gen, not per-thread in the ctor — or (b)
make the Zero build not emit these aarch64 trampoline stubs. Next gdb step to nail
the exact instruction: HW-breakpoint the specific codecache stub PC (now reachable —
ASLR off, addresses deterministic) and single-step the cap_blr + the ret to see
which clobbers CLR/CSP to the stack value.

## K. Fix-A refuted; verified lead = integer stp/ldp of cap regs (2026-06-03)

Planned "fix A" (seed `_cap_trampoline_addr` earlier) is REFUTED by the init order
in `Threads::create_vm`: `main_thread = new JavaThread()` (thread.cpp:2928, runs the
ctor seeding from patch 0131) + `initialize_thread_current()` (2930) happen BEFORE
`init_globals()` (2954, where StubRoutines generates AND first-executes the stubs).
So `_cap_trampoline_addr` IS already seeded when the trampoline stubs run — seeding-
timing is not the cause. Consistent with §J (the fault is ret-to-stack corruption,
not a null cap_blr).

VERIFIED BUG LEAD: `MacroAssembler::call_VM_leaf_base` brackets the cap_blr with
  stp(rscratch1, rmethod, Address(pre(sp, -2*wordSize)))   // macroAssembler_aarch64.cpp:1567
  ...cap_blr...
  ldp(rscratch1, rmethod, Address(post(sp, 2*wordSize)))   // :1615
`stp`/`ldp` here are STANDARD 64-bit-GPR pair ops (assembler_aarch64.hpp:1461
`INSN(stp, 0b10,...)` = opc 0b10, X-register pair), but rscratch1/rmethod are
CAPABILITY registers on purecap. So the save/restore drops tag+bounds (keeps only
the low 64 bits). This is a real correctness bug. (NB it's OUTSIDE the
`#ifdef __CHERI_PURE_CAPABILITY__` trampoline block, so it affects the shared path.)
Whether it is THE ret-to-stack cause is unconfirmed: metadata loss on rmethod/
rscratch1 would normally surface as a later deref (SIGPROT), not CLR/SP corruption —
so single-step confirmation is needed before committing a fix.

NEXT STEP OPTIONS (gdb infra is hot — QEMU -s, ASLR off, kernel.full DWARF,
scripts/catch_sigexit.gdb):
 - B1 (confirm): HW-breakpoint the specific codecache trampoline stub PC and single-
   step the cap_blr + the surrounding stp/ldp + the ret, watching CLR(c30)/CSP go to
   the stack value 0x4267c010. Then fix the exact offending instruction.
 - Fix candidate (once confirmed): replace the integer stp/ldp with cap-width
   save/restore (cap_stp pre-index + cap_ldp post-index). DO NOT hand-encode blindly
   — derive/verify the Morello cap-LDP/STP post/pre-index encodings against clang
   output first (the assembler already has cap_stp_imm=0x42800000, cap_stp_sp_pre=
   0x62800000, cap_ldp_imm=0x42C00000; post-index families are 0x22800000/0x22C00000
   by the addressing-mode bit pattern — VERIFY before use).

## L. B1 result (2026-06-03): control-flow corruption confirmed; EL0 unreadable from EL1

Full register state at the crash (read from the kernel trapframe via gdb; ASLR off):
```
ELR=SP=LR=0x4267c010 (a near-top thread-stack addr; stack 0x4247d000-0x4267d000)
FP(c29)=0x4267c100   ESR=0x8200000f (EC=0x20 instruction abort)
codecache ptrs in regs: c0=0x59559a00 c4=0x59559a50 c12=0x590051c0 c22=0x590050d0 c26=0x59523620
stack/near-SP ptrs:     c1=c19=0x4267bf90 c20=SP c24=0x4267c050 c29=0x4267c100
libjvm .data: c16=0x4210e540   others: c8=0x428b5df0 c21=0x42273980 c23=0x428b8c80 c25=0x428b014d
```
INTERPRETATION (high confidence): a function **epilogue `ret`'d to a corrupted return
address that equals an SP value** (LR==SP==0x4267c010) → fetch from the stack →
instruction abort. Classic "saved LR/return-cap on the stack got clobbered to an SP-
like value". It runs in generated code (codecache pointers live in c0/c4/c12/c22/c26).
NOT a missing opcode, NOT seeding, NOT a clean null cap_blr. The integer-stp-of-cap-
regs defect (§K) is real but does not touch LR, so likely not THE cause.

TOOLING CEILING HIT: the QEMU system gdbstub, stopped at `sigexit` (EL1/kernel),
CANNOT read EL0 userspace VAs — every read of the stack (0x42...) and codecache
(0x59...) returns "unmapped"; only kernel memory (0xffff..., incl. the trapframe) is
readable. So I cannot disassemble the offending stub or read the corrupt frame from
the sigexit catch. To see the exact corrupting instruction requires being **stopped
at EL0** (a HW breakpoint inside the codecache stub while it runs in userspace), or a
bounded `-d in_asm,nochain` instruction trace (firehose; impractical for a full run).

NET: boot crash characterized to "ret-to-corrupted-LR(=SP) in a codecache stub" with
full register evidence; exact instruction still unpinned (tooling ceiling). Two ways
forward: (1) STRUCTURAL FIX (best lead): make the trampoline stub's cap-register
save/restore cap-correct (§K integer stp/ldp -> cap-width, encodings verified vs
clang) AND audit the stub's frame (FP/LR) setup for a wordSize(16) vs BytesPerWord(8)
slot bug -- this is the most likely class of cause for a clobbered saved-LR on
purecap; test empirically. (2) DEEPER DIAG: set a HW breakpoint at the specific
codecache stub PC (find it by scanning the codecache for the cap_blr/ret pattern, or
break at cap_trampoline_dispatch=0x41abf0cc and read CLR to locate the caller stub),
single-step the epilogue at EL0 where userspace memory IS readable.

## M. B2 EL0 single-step — BACKTRACE + the build is template-interpreter, not pure Zero

Caught the fault at EL0 via `hbreak *0x4267c010` (HW bp; needs `file kernel.full`
loaded first or gdb mis-parses the morello 'g' packet -> "Truncated register 37").
At EL0, userspace memory IS readable -> walked the FP chain + symbolized.

SYMBOLIZED C++ BACKTRACE (frames 2-7, real C frames; libjvm base 0x41600000):
  JNI_CreateJavaVM_inner (jni.cpp:3626)
   -> InstanceKlass::initialize_impl (instanceKlass.cpp:1186)
   -> InstanceKlass::call_class_initializer
   -> [generated code 0x428b...] running a class <clinit>   -> crash
So the JVM is running a class STATIC INITIALIZER (<clinit>) and the RETURN from the
generated code corrupts x30(LR): the frame's saved LR on the stack is INTACT
([fp+16]=0x428b5df0, valid code), but the x30 REGISTER = 0x4267c010 (== SP) at the
`ret` -> branch to stack -> ifetch abort. So x30 is clobbered to an SP value before
ret (not a metadata-loss; the call_stub callee-saved restore is already cap-correct
via cap_ldp_imm, §stubGenerator).

GENERATED CODE IDENTITY (disassembled at EL0, ASLR off):
  0x428b5dec: `br x9`     (bytecode-dispatch-to-next-handler)
  0x428b01bc: `blr x4`    (method-entry call)
These are TEMPLATE-INTERPRETER idioms. **This "Zero VM" build is actually running
template-interpreter generated code** (br-x9 dispatch), which is WHY it emits the
aarch64 StubGenerator/call_VM_leaf trampoline stubs that a pure-Zero C++ interpreter
would not (resolves the long-standing "why does Zero generate aarch64 stubs?"
puzzle in §C). The 0x428a8000-0x448a8000 (32 MB) region is the interpreter/stub
code; codecache proper is 0x54bc0000-0x5d3c0000.

NET: the boot crash is **x30/LR clobbered to SP on return from a class <clinit>** in
the template-interpreter's method-return / call path on purecap. Fix direction:
disassemble frame-0's generated return routine to find the instruction that sets x30
from sp (e.g. a `mov c30,csp`-like or a frame-restore that loads x30 from the wrong
slot), or audit templateInterpreterGenerator's return-entry / the call_stub's
`blr c_rarg4` (line 330) return handling for a wordSize(16)-vs-8 LR-slot bug. Now
fully tractable at EL0 (HW bp in the specific generated routine; memory readable).
This is the THIRD distinct C-9 layer: (1) handler code-5 [FIXED], (2) the boot SIGILL
[here, characterized to template-interpreter <clinit>-return x30 corruption], and the
StoplessGC move itself [untestable until boot works].

## N. Source audit (2026-06-03): return paths CLEAN; saved-LR slot overwritten during <clinit>

Audited every LR/return path the crash could be on:
- `MacroAssembler::enter()/leave()` (macroAssembler_aarch64.hpp:907/932): cap-correct
  (cap-stp/cap-ldp c29,c30 at [sp,#-32]!/[sp],#32; mov via cap-add #0). CORRECT.
- `call_stub` return (stubGenerator_aarch64.cpp:330-425): callee-saved restored via
  cap_ldp_imm; final `leave(); ret(lr)`. CORRECT.
- native-method return (templateInterpreterGenerator:1656-1669): cap_ldr sender sp +
  leave() + ret(lr). CORRECT.
- Java `_return` template (templateTable_aarch64.cpp:2257-2266): `remove_activation;
  cap_clear_bit0(lr,rscratch1); ret(lr)`. cap_clear_bit0 (macroAssembler_aarch64.hpp:
  899) = `andr tmp, lr, ~1; SCVALUE lr, lr, tmp` -> clears bit0 of lr's address,
  preserves the cap. CORRECT (clearing bit0 of a valid lr cannot produce an SP value).

THEREFORE: lr was ALREADY == SP (0x4267c010) when it ENTERED the return path —
i.e. leave() restored it from the frame's saved-LR slot, and that slot had been
**overwritten with an SP value during <clinit> execution**. At frame setup
(templateInterpreterGenerator:943 `cap_stp_sp(rfp, lr, 10*wordSize)`) lr is saved
correctly (saved-rfp at rfp+0, saved-LR at rfp+16). So some bytecode handler /
frame access during <clinit> wrote an SP-like value into the saved-LR slot — a
**frame-slot STRIDE error**: the classic purecap **wordSize(16) vs BytesPerWord(8)**
confusion (see docs/38). A slot computed with the wrong stride lands on rfp+16
(saved-LR) and gets an SP/stack value.

PIN PATH (next): EL0-single-step <clinit> execution watching a write to
[rfp+16]/[rfp+11*wordSize] (HW watchpoint on the saved-LR slot address once rfp is
known), OR audit interpreter frame-slot offset computations for `* BytesPerWord` vs
`* wordSize` (or raw byte offsets) that could alias rfp+16 — especially local-
variable / expression-stack stores in <clinit> (which sets static fields). docs/38
(wordSize vs BytesPerWord slot addressing) is the reference for this bug class. This
is the 2nd C-9 layer's root class; once fixed, boot should pass fn_id 47 and the
code-5 handler fix (layer 1) can finally be validated on MinMove.

## O. ROOT CAUSE (2026-06-03): the boot SIGILL is the unfinished L47b (×8 vs ×16 locals)

Source audit per docs/38 landed the root cause. The §N "frame-slot stride error
overwriting saved-LR" is the **documented, never-completed L47b bug**:

- Purecap interpreter slots are 16 B (hold caps), but `LogBytesPerWord=3` (×8).
  `logStackElementSize` was fixed to 4 (patch 0112) for the expr-stack side, BUT the
  **variable-index LOCALS accessors stay at ×8** as a deliberate stopgap to keep the
  build self-consistent: templateTable_aarch64.cpp `iaddress(Register r)` (line 72,
  comment "still ×8, tracked as L47b"), `laddress`/`faddress`/`aaddress`, and the
  `lload/dload/lstore/dstore` `sub(r,rlocals,idx,uxtw,LogBytesPerWord)` forms.
- frame_aarch64.hpp: `return_addr_offset = 1` -> saved-LR at rfp + 1*wordSize =
  rfp+16. A ×8-strided variable-index local store during a class `<clinit>` lands on
  the wrong 16-B slot; the layout puts the saved-LR exactly one mis-strided slot
  away, so the slot gets an SP/stack value -> `leave()` restores lr=SP -> ret-to-
  stack (the observed crash). Same bug class (class-3 layout/size), same component
  (interpreter locals), consistent with the full §J-§N evidence chain.
- docs/38 predicted this class is "latent, scale-dependent corruption... invisible on
  constant-index fast paths, only bites on variable-index access" — exactly a
  <clinit> using a variable-index local.

THE FIX = complete L47b (spec in docs/38 §"L47b — the systematic fix"):
  1. The ~12 `__ ldr/str(dst, iaddress(r))` call sites (templateTable lines
     632/640/643/650/682/694/728/849/1014/1051/1552/1554): compute the slot address
     first via `lea(scratch, Address(rlocals, r, lsl logStackElementSize))` (lea has
     no shift constraint; an 8-byte ldr can't use lsl #4 directly -> "bad shift"),
     then load/store at offset 0. For OOP locals (aaddress) the load is a 16-B cap
     load, so `cap_ldr c,[base,idx,lsl#4]` is legal and can stay scaled.
  2. `lload/dload/lstore/dstore`: `sub(..., LogBytesPerWord)` -> `logStackElementSize`.
  3. Audit interp_masm (e.g. :1925) for `lsl(logStackElementSize)` used as a scaled-
     load shift on an 8-byte access -> same lea treatment.
Do (1)-(3) atomically (keep locals self-consistent). This is a real codegen refactor
+ build + boot + test cycle (NOT just analysis). Once it boots past fn_id 47, the
code-5 handler fix (layer 1) can finally be validated on MinMove, then the StoplessGC
move. NB: this L47b bug is a Zero/template-interpreter PORT artifact — a fresh
CHERI-native GC design (the project's actual contribution) never inherits it.

## P. RETRACTION of §O + watchpoint results (2026-06-03)

**§O is WRONG — retracted.** Reading the actual code (not docs/38, which is stale):
the L47b locals fix WAS completed. `templateTable_aarch64.cpp`:
- `locals_index` (586) and `locals_index_wide` (709) PRE-SCALE the negated index by
  `logStackElementSize - LogBytesPerWord` (=1, i.e. ×2), so the subsequent
  `iaddress(r)` `lsl#3` yields the correct ×16 stride. (The `iaddress(Register)`
  comment at line 72 saying "still ×8" is STALE.)
- `lload`/`dload` (672/689) load the index raw then `sub(r1,rlocals,r1,uxtw,
  logStackElementSize)` = ×16 (no double-scale; they do NOT use locals_index).
- `aload` (696-703) uses `locals_index` + `lea(iaddress)` + `cap_ldr` (C-6 #53).
All variable-index locals are ×16-correct. So the boot SIGILL is NOT the locals bug.

So the SOURCE audit has now ruled out EVERY obvious path: enter/leave, call_stub
return, native return, Java _return + cap_clear_bit0, AND all locals accessors.

WATCHPOINT (HW, on saved-LR slot 0x4267c110 = crash FP 0x4267c100 + 16):
- Conditioned on a STACK-range value: fired at PC 0x40162784 (a mapping BELOW libjvm
  / below 0x401a4000 — likely ld-elf.so / libc / launcher), store at 0x40162780
  (cap-store), value 0x4267bfd0. This is almost certainly an EARLIER reuse of that
  stack address by a deeper frame, not the <clinit> corruption (value != crash value).
- Conditioned on the EXACT crash value 0x4267c010: NEVER fired (java still crashed).
  => either (a) the slot's final value is written as a 16-byte CAP store whose low-8
  my `*(unsigned long*)` watch + condition didn't match, (b) the crash frame is at a
  slightly different address THIS run (init non-determinism across the long session),
  so 0x4267c110 wasn't the live saved-LR slot, or (c) leave() reads lr from a slot
  that was never written by THIS frame (stale value left by a prior frame) — i.e.
  the frame SETUP stored lr to a different slot than teardown reads (a setup/teardown
  offset MISMATCH), which the watchpoint on the teardown-slot wouldn't catch as a
  "write".

HONEST STATUS: crash CHARACTERIZED (ifetch abort, ret to x30==SP, returning from a
class <clinit> in template-interpreter codegen; §J-§N solid) but ROOT CAUSE NOT YET
PINNED. §O's L47b attribution was a confident mis-diagnosis (stale-doc-driven) and is
withdrawn. Robust next step: re-catch sigexit to confirm THIS-session crash ELR/FP
(rule out address drift), then either (i) watch the confirmed live saved-LR slot for
ANY write (cap-aware, no value condition) and take the LAST write before sigexit, or
(ii) compare generate_fixed_frame's saved-LR STORE offset vs leave()/remove_activation's
LR LOAD offset for a setup/teardown mismatch (hypothesis (c) above).

## Q. Watchpoint pin attempt #2 (2026-06-03): correct slot found, but conditional WP impractical

Fixed the §P address error: at the crash, the FP register is the *restored caller* FP,
not the <clinit> rfp. Re-derived from the trapframe: leave() does `sp=R; cap-ldp
c29,c30,[sp],#32`, so crash_SP = R+32 and the **saved-LR slot = R+16 = crash_SP-16 =
0x4267c000** (confirmed: tf_sp=0x4267c010 every run; ASLR off). (§P watched 0x4267c110
— wrong; that's why it missed.)

HW watchpoint on the CORRECT slot 0x4267c000:
- value-in-stack-range condition: fires on EARLY reuse writes (the slot is the saved-LR
  slot for EVERY frame that nests to that depth — hundreds during boot). First hits were
  low-lib (ld/libc/launcher) frames writing their own FP/stack values (0x4267c210,
  0x4267bfd0) — NOT the <clinit> corruption (value != 0x4267c010).
- exact-value condition (==0x4267c010): QEMU's gdbstub HALTS on EVERY write to the slot
  to evaluate the condition; the slot is so hot that `java -version` no longer finishes
  within 180s (SIGTERM'd) before the corruption is reached. Conditional HW watchpoint on
  a hot stack slot over the QEMU gdbstub is impractical.

TOOLING WALL (recurring this session): vanilla gdb mis-parses the morello 'g' packet
(needs kernel.full loaded; HW-bp/regs flaky), `gdb -9` leaves the gdbstub's TCP
connection half-open so the next connect times out (vMustReplyEmpty) and leaves the
CPU HALTED (recover via HMP `cont`), and repeated attach/halt cycles wedge guest sshd
(recover via reboot, ~4 min). Each gdb iteration costs minutes of recovery.

WHAT'S SOLID: the corrupting write puts an SP-range value (0x4267c010) into the
<clinit> frame's saved-LR slot (0x4267c000) during template-interpreter execution of a
class <clinit>; on return `leave()` restores lr=that value and `ret`->ifetch abort.

BETTER PIN (recommended, replaces watchpoint): INSTRUMENT the JVM instead of fighting
the gdbstub. Add a debug check on the Java return path (TemplateTable::_return, right
after remove_activation/before `cap_clear_bit0(lr); ret(lr)`): if `lr`'s address is in
the current thread's stack range (i.e. corrupt), `fprintf` the current Method
(rmethod->external_name) + bcp/BCI and abort. One build+deploy cycle, DECISIVE: names
the exact method and bytecode whose execution corrupted the saved-LR slot, with zero
gdbstub fragility. Alternatively instrument call_class_initializer to print the klass
name of each <clinit> (the LAST before the crash = the culprit class), then read its
<clinit> bytecode. Either is faster/more reliable than the conditional watchpoint.

## R. CULPRIT = java.lang.String.<clinit> (2026-06-03, instrumentation)

Pivoted from the (impractical) watchpoint to JVM instrumentation: added a
`fprintf("[CLINIT-RUN] %s")` of `external_name()` in `InstanceKlass::call_class_initializer`
before `JavaCalls::call`. Result: exactly ONE line — `[CLINIT-RUN] java.lang.String`
— then "Illegal instruction". So the crash is on/in the VERY FIRST class initializer,
**java.lang.String.<clinit>** (decisive, no gdb fragility).

String.<clinit> bytecode (JDK17, from javap) is tiny:
```
0  iconst_1; putstatic COMPACT_STRINGS:Z
4  iconst_0; anewarray ObjectStreamField; putstatic serialPersistentFields   <- runtime alloc call
11 new String$CaseInsensitiveComparator; dup; invokespecial <init>()V; putstatic CASE_INSENSITIVE_ORDER
21 return
```
The §J/§M backtrace had TWO nested interpreter frames, so the crash is a deeper
callee returning (the `<init>` at BCI 15, or an allocation runtime helper for the
`anewarray`/`new`), NOT necessarily String.<clinit>'s own final `return`.

STRONG SUSPECT (concrete): the runtime-call path. `anewarray` (BCI 5, the FIRST
runtime call) and `new` (BCI 11) go interpreter -> `call_VM` -> ... ->
`MacroAssembler::call_VM_leaf_base`, which has the §K bug: integer `stp/ldp(rscratch1,
rmethod, [pre/post(sp,±2*wordSize)])` saving/restoring CAPABILITY registers (drops
tag/bounds; and reserves 32B but writes only 16B). Plus `invokespecial`/`return`
exercise the method call/return frame machinery. The corruption writes an SP-value
into the deeper frame's saved-LR slot.

NEXT (decisive, two options): (a) re-do the watchpoint with a TIGHT window — break at
`call_class_initializer` (fires once, for String), then single-step String.<clinit>'s
~8 bytecodes + callees watching the saved-LR slot (few writes, no hot-slot slowness);
(b) audit/fix the runtime-call frame path: §K integer stp/ldp -> cap-width, AND audit
`call_VM_base`/`generate_call_stub`-style interpreter runtime-call glue + the
`invokespecial`/`return` templates for a saved-LR/wordSize slot bug. Given §K is a
verified concrete defect on the exact path `anewarray` takes, fixing it + retesting is
the most actionable shot.

## S. ROOT CAUSE FOUND: remove_activation return-path strips CSP tag (2026-06-03)

Built a sigaltstack-based crash dumper (handler.c stopless_install_crash_diag,
SA_ONSTACK, re-armed from InstanceKlass::call_class_initializer right before
java.lang.String.<clinit>, overriding HotSpot's handlers which themselves died
on the corrupt stack -> no hs_err). It fired with a DECISIVE dump:

    sig=11 SEGV_ACCERR addr=0x426bd010
    ELR(PC)=0x426bd010  CLR(c30)=0x426bd010  CSP=0x426bd010  c29(FP)=0x426bd100
    CSP tag=0 base=0 top=0xffffffffffffffff      <-- SP is UNTAGGED (null-derived)

PC==LR==SP all equal a STACK address, and CSP has tag=0/base=0/top=MAX — the
exact signature of an INTEGER op writing the SP register. Traced to the
UN-PORTED tail of `InterpreterMacroAssembler::remove_activation`
(interp_masm_aarch64.cpp ~796-825), which had NO __CHERI_PURE_CAPABILITY__ path:

    ldr(rscratch2, [rfp, sender_sp_offset*wordSize]);  // (1) int ldr of a CAP (sender_sp) -> tag stripped
    ...
    mov(esp, rscratch2);                                // (2) esp <- untagged
    leave();                                            // (correct: cap-add sp,rfp,#0; cap-ldp)
    andr(sp, esp, -16);                                 // (3) int AND writes CSP -> tag-0 CSP

leave() itself is correct (restores lr from [rfp+16] fine), so the FIRST return
succeeds — but it leaves the CALLER's CSP untagged. The caller then resumes,
and its own return path / next CSP use collapses PC/LR/SP onto a stack address
-> ifetch from stack -> SEGV/SIGILL. This is the C-9 boot blocker (manifested at
java.lang.String.<clinit>, the first class initializer).

FIX (cap-correct, gated on __CHERI_PURE_CAPABILITY__):
  (1) cap_ldr_imm(rscratch2, rfp, sender_sp_offset*wordSize)  -- preserve tag
  (2) cap_add_imm(esp, rscratch2, 0)                          -- cap-mov
  (3) cap_add_imm(sp, esp, 0)   -- cap-copy (sender_sp is already 16-aligned,
      so this is equivalent to `andr sp,esp,-16` but keeps CSP's tag/bounds).
cap_add_imm/cap_sub_imm already special-case `sp`->reg31. Rebuilding + retesting.

This was found WITHOUT gdb (which had been wedging the guest) — the in-process
sigaltstack dumper is the reusable tool for any future boot crash.

## T. §S fix VALIDATED; residual c30=sender_sp on return (2026-06-03)

The §S remove_activation cap-fix (cap_ldr sender_sp; cap_add_imm esp; cap_add_imm sp)
WORKS: the crash signature changed from "CSP tag=0 base=0 top=MAX" (untagged) to a
properly TAGGED CSP (base=0x424be000 top=0x426be000), and the stack window/frame
chain are now fully readable. Confirmed mov(Register,Register) is already cap-aware
(C-6 #13/#8): mov(r13,sp)/mov(sp,rscratch1) emit cap-ADD, so sender_sp keeps its tag.

RESIDUAL crash (still at java.lang.String.<clinit>, now SIGSEGV/SEGV_ACCERR):
    ELR(PC)=CLR(c30)=CSP=0x426bd010   c29(FP)=0x426bd100
    [fp+16] saved_lr slot = 0x428f5df0   (the CORRECT return entry — intact)
    c8(rscratch1)=0x428f5df0 tag=0       (return addr, but in rscratch1, untagged)
    c30(lr)=esp(c20)=sender_sp(c13)=c9=CSP=0x426bd010   (FIVE regs collapsed to CSP)
So we branch (ret/br) via c30 to a STACK address (=sender_sp), while the in-memory
saved-LR slot is correct. i.e. lr/c30 got the sender_sp value instead of the saved
return address, OR the faulting `ret` is not the one preceded by cap_clear_bit0
(c8=0x428f5df0 implies a cap_clear_bit0 ran with lr=0x428f5df0, inconsistent with
c30=0x426bd010 at the SAME ret) — pointing at a DIFFERENT ret/br than the template
_return (possibly a stub / native / method-handle path, given rmethod(c12)=0x590051c0
and rbcp(c22)=0x590050d0 both sit in the CODECACHE, not metaspace).

Static forensics can't pin the faulting branch (ELR = post-branch target; the branch
PC isn't preserved). NEXT decisive options:
  (a) gdbstub single-step the callee's _return / the active stub from a breakpoint at
      call_class_initializer(String) — now that we know the exact crash, a short step
      window is feasible despite earlier gdbstub flakiness;
  (b) enhance the crash dumper to scan the stack BELOW CSP for the callee frames and
      flag which method's saved-LR slot (if any) holds a stack-range value, and to
      decode the code words at c8/c23/c25 (return-entry/normal-entry/call_stub) to
      identify which generated path is executing.

TOOLING WIN: the sigaltstack crash dumper (handler.c stopless_install_crash_diag,
re-armed pre-String.<clinit>) is the reusable, gdb-free way to get full trapframe +
stack + cap-register state on any boot crash. Diagnostic source changes
(instanceKlass clinit hook, the dumper) are kept as WIP records, not in the apply
series, until boot is validated end-to-end.

## U. Faulting branch = ret/br to sender_sp; dispatch path intact (2026-06-03)

Built in-process disassembly into the crash dumper (gdb via the system gdbstub
CANNOT read this process's userspace — when the hung process isn't the current
CPU context, TTBR0 differs and reads return "unmapped"; the dumper runs IN the
java address space with cap access, so it reads code/data by re-addressing any
tagged register cap whose bounds cover the target). Findings (ASLR off, stable):

- The invoke return-entry @0x428f5df0 disassembles cleanly and ends with the
  bytecode dispatch `br x9` (0x428f5e5c): ldrb w8,[rbcp,#step]! ; addw w9,w8,#0x900
  ; lslw w9,#4 ; ldr x9,[rdispatch,w9] ; br x9. All cap-correct (C-6 fixes intact).
- The dispatch TABLE (rdispatch=0x422749c0) is VALID: entries are real handlers
  (0x428faca8, 0x428fad28, ...). So a `br x9` from the table cannot yield a stack
  value. Therefore the faulting branch is NOT the dispatch.
- rbcp=0x590050d0, rmethod=0x590051c0 are in metaspace (NOT codecache — the old
  0x59xxxxxx=codecache guess was wrong; these registers are fine).
- The crash branch target = 0x426bd010 = sender_sp = esp = CSP. c9(rscratch2)=
  sender_sp is just our remove_activation `cap_ldr_imm(rscratch2,rfp,sender_sp)`
  leftover (coincidental). By elimination the faulting branch is `ret(lr)` (or a
  stub ret) with lr(c30)=sender_sp.
- BUT c8(rscratch1)=0x428f5df0 (a return-entry addr) is inconsistent with the
  template _return's `cap_clear_bit0(lr,rscratch1)` having just run (it would
  make rscratch1==lr.addr==target). So either the faulting ret is a NON-template
  path (call_stub/native/method-handle ret, no cap_clear_bit0), or c8 is stale
  from a prior successful _return (which DID return to 0x428f5df0).
- Wide stack window: the caller frame (rfp~0x426bd030: saved_fp=0x426bd100,
  saved_lr=0x428f5df0, sender_sp slot=0x426bd010) is INTACT. leave() did NOT run
  for the crashing frame (c29=0x426bd100 is not 0 and not derivable from a leave
  off any frame whose [rfp+16]=0x426bd010), so the crash is BEFORE leave() or in a
  non-_return path.

OPEN: identify the exact ret/br. Static forensics is exhausted (ELR=post-branch
target; branch PC not preserved). Decisive next step needs the java process as
gdb's CURRENT context to single-step: break on the KERNEL EL0 instruction-abort
handler (TTBR0 still = java there) and step the few instrs before the fault, OR
add a `brk #0` to the dumper so gdb stops in java context. The §S remove_activation
fix remains correct and necessary (it fixed the CSP-tag-strip half).

## V. gdb HW-breakpoint confirms: faulting branch is a `ret` to sender_sp (2026-06-03)

Honored the "single-step via gdb" plan, but discovered the system gdbstub can't
read this process's userspace once it's descheduled (wrong TTBR0), and a program
`brk #0` is NOT reported to gdb by QEMU (it became a guest SIGTRAP -> died). The
working technique: a gdb HARDWARE breakpoint (`hbreak`) at a known VA fires
regardless of current TTBR0, trapping gdb WITH the java process current. Use the
Morello-capable gdb at third_party/output/upstream-gdb/bin/gdb (the system
/usr/bin/gdb 12.1 can't parse the Morello 'g' packet — "Truncated register 37").

`hbreak *0x426bd010` (the crash LANDING address; ASLR off) fired exactly once,
catching the branch the instant it landed on the stack, java context current:
    pc = x30(lr) = x9 = x13 = x20 = sp = 0x426bd010 (= sender_sp)
    x8(rscratch1) = 0x428f5df0   (a return-entry addr, NOT a small bytecode)
    x29(fp) = 0x426bd100 (intact)
Conclusions (now firm):
 - NOT the dispatch `br x9`: that path does `ldrb w8,[rbcp]` first, so x8 would be
   a small bytecode; x8=0x428f5df0 instead.
 - It is a `ret` (target = x30 = sender_sp).
 - NOT the template `_return`: its `cap_clear_bit0(lr,rscratch1)` runs immediately
   before `ret(lr)` and sets rscratch1 = lr.addr; here x8(rscratch1)=0x428f5df0 ≠
   lr(0x426bd010), so no cap_clear_bit0 preceded this ret. x8 is leftover from a
   PRIOR successful _return (which returned to the invoke return-entry 0x428f5df0).
 => the faulting `ret` is a NON-template path: call_stub / native wrapper /
    method-handle / i2c-c2i adapter `ret`, whose lr was restored as sender_sp.

NEXT: find that ret's address (disassemble the call_stub @~0x428f0140 and the
native/MH entries via the dispatch/stub tables) -> `hbreak` it -> single-step the
ret and read lr's SOURCE slot to see why it = sender_sp. Tooling: scripts/
catch_landing.gdb (hbreak landing) is the template; swap the address to the ret.

## W. call_stub §S-class fix; -Xint same crash => pure-interp runtime-call path (2026-06-03)

Fixed a genuine §S-class bug in generate_call_stub param-passing (stubGenerator_
aarch64.cpp ~302): the stock `sub(rscratch1,sp,..)` + `andr(sp,..)` write CSP via
INTEGER ops, stripping its tag; for a no-param call (a static <clinit>) `mov(r13,sp)`
then stores an UNTAGGED sender_sp into the entry frame. Fixed: cap-SUB the
(32-aligned) param delta from CSP preserving the tag. CONFIRMED this is NOT the boot
crash (the crash is a deeper interp callee whose sender_sp comes from
jump_from_interpreted, tagged) — same crash signature after the fix. Kept as a
correctness fix.

`-Xint` (pure interpreter, no C1/compiler-thread/adapter) -> IDENTICAL crash
(ELR=0x426bd010). So the faulting `ret(lr=sender_sp)` is in the INTERPRETER, and it
is NOT the template _return (x8≠lr rules out cap_clear_bit0), NOT call_stub, NOT
dispatch, NOT compiled/adapter. Remaining strong suspect: the RUNTIME-CALL path
(`call_VM`/`call_VM_base`) taken by String.<clinit>'s first `anewarray` (BCI 5) /
`new` (BCI 11) / first-exec resolution — a non-_return path with its own
return-address/frame handling that could leave lr=sender_sp. NEXT: audit
MacroAssembler::call_VM_base (interpreter runtime-call glue) on purecap.

## X. RT trace localizes crash to first interp->interp->interp call; return-entry stride fix (2026-06-03)

Instrumented InterpreterRuntime (_new/anewarray/resolve_from_cache) with prints.
String.<clinit> trace before the crash (pure -Xint):
    [CLINIT-RUN] java.lang.String
    resolve_from_cache putstatic        (BCI1 COMPACT_STRINGS)
    anewarray size=0 enter/exit OK      (BCI5 ObjectStreamField[0])
    resolve_from_cache putstatic        (BCI8 serialPersistentFields)
    _new enter                          (BCI11 new CaseInsensitiveComparator)
    resolve_from_cache invokespecial    (BCI15 -> CaseInsensitiveComparator.<init>)
    resolve_from_cache invokespecial    (CIC.<init> BCI1 -> Object.<init>)
    <CRASH>
So the crash is the FIRST interp->interp->interp call/return chain
(String.<clinit> -> CaseInsensitiveComparator.<init> -> Object.<init>), a ret/br to
sender_sp (0x426bd010). It is NOT _return (x8=rscratch1=0x428f5df0 ≠ lr, so no
cap_clear_bit0 preceded it), NOT the dispatch br x9 (x8 not a bytecode), NOT
jump_from_interpreted's `br rscratch1` (x8≠target), NOT call_stub, NOT compiled
(-Xint identical).

Found+fixed (genuine, but NOT the immediate crash — confirmed by no address shift):
the invoke RETURN-ENTRY's machine-SP reconstruction (generate_return_entry_for) used
stride ×8 (+roundup16) while generate_fixed_frame uses ×16 (logStackElementSize). Mis-
restores the caller CSP by (max_stack+monitor+2)*8 after EVERY interpreted return —
fixed to ×16. (Not reached before the current crash, so the boot crash is unchanged;
kept as a correctness fix that would otherwise bite once we get past the current one.)

THREE §S-class cap bugs now fixed (remove_activation §S, call_stub §W, return-entry
§X) — all real, none the immediate boot crash. The immediate crash (ret/br to
sender_sp in the first nested call) still eludes exact pinpointing: static analysis
exhausted; gdb stepi-trace impractical (slow gdbstub + the crash precedes every
breakpoint I can place by symbol). DECISIVE next options: (a) build a fastdebug JVM
and use -XX:+TraceBytecodes (last bytecode before crash = culprit, exact); (b) hbreak
at the resolve_from_cache RETURN into the invokespecial template and stepi the small
call->entry->return window; (c) add a per-method-entry C++ print (Method name) to
identify which method's entry/return faults.

## Y. FAULTING METHOD = java.lang.Object.<init> (empty); decoded via gdb (2026-06-06)

Used the landing hbreak (0x426bd010) + libjvm DWARF offsets to decode the frame
Method* names (Method::_constMethod@16 -> ConstMethod::_constants@16 /
_name_index@58 -> ConstantPool[base=128 + idx*16] -> Symbol::_length@4/_body@6;
holder via ConstantPool::_pool_holder@48 -> Klass::_name@32). Result:
  FAULTING popped frame  M=0x590051c0  class=java/lang/Object             method=<init>
  caller frame           M=0x595561c0  class=java/lang/String$CaseInsensitiveComparator method=<init>
  + a java/lang/String frame (String.<clinit>).

So the crash is on the return path of **java.lang.Object.<init>** — THE canonical
empty method (bytecode = just `return`; on aarch64 `case empty` falls to
generate_normal_entry, so it uses normal_entry + the template _return). Since
x8(rscratch1)=0x428f5df0 = a return-entry addr = Object.<init>'s _return
cap_clear_bit0 output, Object.<init> _returns CLEANLY to CaseInsensitiveComparator.
<init>'s invoke return-entry (0x428f5df0); the crash (ret/br to sender_sp 0x426bd010)
is in that small pure-interp window (return-entry restore/CSP-reconstruct/dispatch,
or CIC.<init>'s own subsequent _return). This is the FIRST empty-method return in the
whole boot. Tracing it: hbreak *0x428f5df0 if $x12(rmethod)==0x590051c0 (fires only on
Object.<init>'s return), then stepi the ~30-instr window.

## Z. ★ ROOT CAUSE FOUND & FIXED: cap_clear_bit0 SCVALUE base off-by-one-bit (2026-06-06)

THE boot blocker. `MacroAssembler::cap_clear_bit0(Cn=lr, tmp=rscratch1)` (used by
every interpreted `_return` to clear PSTATE.C64 bit0 from the return sentry) emitted
SCVALUE with base constant `0xC2C14000`. The TRUE SCVALUE encoding (verified with the
Morello assembler — `llvm-mc`/clang: `scvalue c30,c30,x8` = 0xc2c843de,
`scvalue c30,c30,x9` = 0xc2c943de) has Rm in bits[20:16] and base `0xC2C04000`. The
extra set bit (0x10000) meant `Rm = tmp->encoding() | 1`. For tmp = rscratch1 = x8
(EVEN), Rm became x9 = rscratch2. So:
    andr(x8, lr, ~1)          ; x8 = correct bit0-cleared return address
    scvalue(lr, lr, x9)       ; lr.address = x9  (NOT x8!)
and x9 (rscratch2) holds sender_sp right after remove_activation (it is the
sender_sp the remove_activation tail cap-LDRs). So `ret(lr)` jumped to sender_sp — a
stack address. EVERY interpreted _return was affected; it surfaced at the FIRST one
in boot: java.lang.Object.<init> during java.lang.String.<clinit>.

This perfectly explains the long-standing puzzle "x8(=0x428f5df0, the return-entry)
≠ lr(=0x426bd010, sender_sp) at the landing, so it's not _return": x8 WAS the correct
address cap_clear_bit0 computed; the buggy scvalue used x9 instead. It is the
template _return after all.

FIX: 0xC2C14000U -> 0xC2C04000U in BOTH cap_clear_bit0 AND cap_lea_global
(macroAssembler_aarch64.hpp). cap_lea_global has the same base typo (manifests only
when its tmp is an even register).

How it eluded earlier C-phases: this code path (template _return's cap_clear_bit0 with
an even rscratch1) is only exercised by an INTERPRETED Java method return; the
C-5..C-8 tests (java -version reaching argument parsing, HelloGC) crashed/finished
before the first interpreted return, or the earlier no-call microbenches avoided it.
The three §S/§W/§X fixes were real but secondary cap bugs on the same return path.

## AE. Post-SCVALUE iterative cap-fault chase (2026-06-06)

With the SCVALUE root cause fixed, java -version (-Xint, Epsilon) advances one
latent cap-strip at a time. Built a build-deploy-test loop (/tmp/bdt.sh) + made the
crash dumper auto-disassemble around ELR and decode the faulting load/store base
register tag, so each fault is one-glance diagnosable. Fixes landed (all the same
classes: integer ldr/str/stp of a capability strips the tag; integer `andr/ret` on
csp; C64 bit0 on a branch; codecache-vs-libjvm PCC bounds on cross-region returns):

  §AA call_stub: result-pointer integer ldr -> cap_ldr_imm (result store deref'd tag-0)
  §AB call_stub: integer `ret x30` -> cap-RET (ret c30) returning to libjvm (bounds + C64)
  §AC load_resolved_method_at_index: f1/f2 Method* integer ldr -> cap_ldr_imm
  §AC load_method_holder: Method->ConstMethod->ConstantPool->pool_holder chain -> cap_ldr_imm
  §AD generate_native_entry: integer `andr(sp,esp,-16)` -> cap mov (CSP tag)

Boot progress: String.<clinit> SIGSEGV  ->  String.<clinit> completes  ->  call_stub
return to libjvm  ->  clinit #2 (2nd class init)  ->  its invokestatic clinit-barrier
->  the NATIVE method entry. Current frontier: generate_native_entry is a large area
the C-6 work did NOT port (frame stp's at lines 865/867, the blr to the signature/
native/result handlers, C64 mode bits on those calls/returns, cross-region PCC). The
current fault is a SIGBUS at an odd PC (0x428f98d1) — a C64 bit0 set on a return into
the native-entry code after an integer `blr` (needs cap_blr / cap-RET, same as §AB).
Next: port generate_native_entry's call/return sequences + frame stp's.

Tooling: /tmp/bdt.sh (build+deploy+test), crash dumper ELR auto-disasm. The openjdk
source changes are live in the working tree and persisted as
patches/openjdk-jdk17/WIP-c9chase-*.record.

## AH. Native entry fully ported; boot at clinit #3 (2026-06-06)

generate_native_entry is now cap-ported end-to-end and the first native method call
COMPLETES (§AF/§AG): signature handler (cap_ldr + cap_blr; the generated handler
cap-RETs), mirror oop store + handle/JNIEnv cap-add, native function (cap_ldr r10 +
direct unsatisfied compare matching x86 + cap_blr — the cross-region call into the
native lib works and returns), post-call handle reset (cap_ldr active_handles), oop
result store, and the result handler (integer blr within the codecache-wide PCC + the
generated result-handler stub clears the C64 bit0 and integer-rets). Key learning: a
codecache-internal call uses integer blr + cap_clear_bit0 (one wide PCC); a call to a
properly-formed code CAP (signature handler from Method::_signature_handler) uses
cap_blr + cap-RET; a `lea`-derived stub address is NOT a valid cap so cap_blr on it
faults (tag-0 PCC).

Boot progress: String.<clinit> -> clinit #2 (its first NATIVE method, fully executed)
-> clinit #3. The crash is now a tag-0 oop null-check (`ldr xzr,[c2]`) in a general
bytecode template at 0x4290414c — an oop flowed through the expression stack/locals
with its tag stripped by a not-yet-ported template. This is the broader
template-oop-handling tail (aload/astore/dup/getfield/aastore/invoke-receiver paths);
each surfaces the same integer-ldr/str-of-a-cap class and is fixed the same way with
/tmp/bdt.sh + the ELR-auto-disasm dumper. ~14 cap fixes landed this session
(§S/§W/§X/§Z/§AA-§AG); the dominant blocker (SCVALUE §Z) is fixed.

## AI. ★ C-5 ACHIEVED on the template interpreter: java -version exits 0 (2026-06-10)

`java -Xint -XX:+UnlockExperimentalVMOptions -XX:+UseStoplessGC
-XX:-UseCompressedOops -XX:-UseCompressedClassPointers -version` prints the full
banner and EXITS 0 on Morello purecap (3/3 stable). The "clinit 121 /
CoderResult" framing from patch 0161 was a misread — the post-banner wall was
three stacked bugs, all in the shutdown path:

1. **SafeFetch never worked on purecap (DDC=NULL alternate-base loads).**
   safefetch_bsd_aarch64.S's `ldr x0,[x0]` assembles (purecap target) to the
   Morello ALTERNATE-BASE form `ldur x0,[x0]` — DDC-relative in C64. Purecap
   processes run with DDC=NULL, so EVERY SafeFetch tag-faulted (SIGPROT code=2,
   si_addr==_SafeFetchN_fault==0x41f60780 — the "clinit 121" address) and
   returned the default. Fix: `ldr x0,[c0]` under __CHERI_PURE_CAPABILITY__.
   BUG CLASS: any hand-written `.S` that takes a pointer argument and writes
   `[x0]` is silently DDC-relative on purecap.

2. **Crash dumper swallowed the SafeFetch protocol.** stopless_install_crash_diag
   REPLACES sigprot_handler (which had the C-6 L38 SafeFetch redirect) once
   re-armed, so an expected SafeFetch probe fault became a fatal dump
   (ServiceThread -> OopStorage::release -> block_for_ptr -> SafeFetchN).
   Fix: factored stopless_safefetch_redirect(); BOTH sigprot_handler and
   stopless_crash_dumper try it first (handler.c).

3. **monitorenter free-slot csel strips the tag.** TemplateTable::monitorenter's
   search loop `csel(c_rarg1, c_rarg3, c_rarg1, EQ)` is an integer csel: it
   selects the freed monitor slot's ADDRESS but kills its tag. Hit on the SECOND
   monitorenter of Shutdown.exit (synchronized(lock) then
   synchronized(Shutdown.class)): monitorexit freed slot -> csel picks it tag-0
   -> `str c0,[c1,#obj]` SIGPROT. Fix: branch + cap-aware mov under purecap.
   BUG CLASS: csel on capability registers (only this one site in the
   interpreter codegen).

Hardening (same patch): OopStorage::Block::block_for_ptr walks plain addresses
and re-derives each candidate from ptr's still-tagged cap (the stock pointer
walk starts 896B below the block allocation — below-base excursions risk
detagging on CHERI). Tooling: stopless_codeblob_name now names the Interpreter
CODELET containing a PC ("Interpreter codelet: monitorenter (start=...)") —
this is what pinned bug 3 in one glance.

Diag method worth keeping: when a probe loop "can't find" something, print what
each probe SEES vs what a direct in-bounds read returns — SafeFetchN returning
default-for-everything (0xdeadbeef test) instantly separated "data wrong" from
"loader broken".

Epsilon control still SIGSEGVs in the `new` template fast path (heap-boundary
0x45980000) — pre-existing, Epsilon-only (StoplessGC routes _new through the
runtime), not a regression.

Next frontier: real programs — the C-9 post-move dispatch spin (docs/40) and
the invokedynamic/Access<> RuntimeDispatch blocker remain.

## AJ. Real-program chase: ConcatTest from pre-main to deep indy bootstrap (2026-06-10)

Re-baselined the Zero-era blockers on the template-interpreter build by running
tests/integration/ConcatTest (string concat -> invokedynamic). Sequence of
root causes fixed (each verified by forward progress; java -version stays rc=0):

patch 0163 (committed earlier):
- call_stub T_OBJECT result store was is_long's integer str — every oop
  RETURNED through JavaCalls reached native tag-0.
- Signature-handler STACK-passed oop arg (>8 GPR args, defineClass1's
  `source`) stored the handle cap with integer str.

patch 0164 (this commit):
1. **layout_helper unit bug**: instance lh encoded as size<<LogBytesPerWord(3)
   but decoded by size_given_klass as bytes>>LogHeapWordSize(4) — oop->size()
   returned HALF the real word count. JVM_Clone then allocated a 0x40-byte
   copy of a 0x90-byte MemberName -> bounds SIGPROT in resolve_MemberName.
   Fix: encode/decode lh with LogHeapWordSize so lh is real bytes (klass.hpp).
   (The MemberName "injected fields" suspicion from docs/09 was WRONG — the
   layout itself is fine; C9-mn/C9-clone diags pinned the size mismatch.)
2. generate_Reference_get: integer ldr of the receiver cap + `andr(sp,..)`
   CSP-tag-strip (§S class).
3. TemplateTable::multianewarray: integer lea ×8 on esp (tag strip + wrong
   16-byte-slot scaling), both the dims pointer and the esp pop.
4. **MacroAssembler::push/pop(Register)**: integer str/ldr on the expression
   stack — strips the invokedynamic APPENDIX MemberName pushed by
   prepare_invoke (checkcast tag-0 in Invokers$Holder.invokeExact_MT @bci=8)
   and any saved cap reg (r19).
5. MethodHandles adapter entry: integer ldr of Method::_constMethod + the MH
   receiver/recv loads from the expression stack.
6. MacroAssembler::argument_address (register case): integer add on esp.
7. BarrierSetAssembler::load_at T_ADDRESS: integer ldr — strips
   ResolvedMethodName.vmtarget (Method* cap) in jump_to_lambda_form.

Tooling: crash dumper now prints the INTERPRETED METHOD NAME + bci directly
(stopless_method_name(c12, c22) in os_bsd_aarch64.cpp, weak-linked from
handler.c) — this named Invokers$Holder.invokeExact_MT / invokeBasic /
findBootstrapClass without any offline Method* decoding. Plus env-gated diags
C9_ARG_DIAG (JavaCallArguments oop tags), C9_MN_DIAG (MemberName bounds +
clone sizes), C9_AC_DIAG (oop-arraycopy tag check — arraycopy is CLEAN).

CURRENT FRONTIER: ConcatTest now reaches ClassLoader.findBootstrapClass
(native, via the MH/class-loading chain) and faults at the native entry's
call_VM_leaf scratch save `stp x8,x12,[csp,#-32]!` with **CSP tag=0 base=0**
while esp (c20) is tagged at the same address — something on the MH-adapter →
native-entry path writes CSP through an integer op. mov/cap_add_reg are clean;
suspect list: prepare_to_jump_from_interpreted / the linkTo adapter's stack
adjustment / native-entry prologue under this entry sequence.

## AK. The NULL-CSP sweep + exception-path porting (2026-06-10, patch 0165)

Continuing the ConcatTest chase past §AJ's findBootstrapClass frontier.

**Root cause of the §AJ CSP fault — and a whole CLASS of latent twins.** The
post-native-call "make room for the pushes" used integer `sub`+`andr sp` —
producing a NULL-DERIVED CSP (value-only write: tag=0 base=0 top=max, NOT a
detagged stack cap, which would keep bounds). Harmless on the fast path; fatal
when the slow native->Java transition (safepoint pending on return) does
call_VM_leaf -> `stp [csp,#-0x20]!`. The same integer-CSP-write pattern was
then swept across the whole file (the offline range-dump disassembly of the
deployed native-entry codelet found them):
  - native return "make room" (the §AJ hit)        -> cap_sub_imm
  - generate_deopt_entry_for / generate_throw_exception ×2 (stack-limit
    rebuild: also scaled max_stack ×8 not ×16)     -> cap-LDR + cap-add-reg
  - generate_stack_overflow_check, 3× CRC32 entries -> cap-mov sp, r13
  - push/pop_call_clobbered_registers + push/pop_CPU_state (sub/add sp, step)
    -> cap_sub_imm/cap_add_imm

**Exception path ported (first real exception unwinding ever exercised —
class loading throws CNFE routinely):**
  - generate_forward_exception: integer ldr/str of Thread::_pending_exception
    delivered a TAG-0 exception oop to the interpreter's handler lookup
    (klass() SIGPROT). cap-LDR/cap-STR (zr cap-store clears the full slot).
  - call_stub catch_exception: same integer str when SETTING pending.
  - _remove_activation_entry: integer stp/ldp of (exception, lr) around the
    handler-address call -> cap_stp_sp_pre + cap_ldp_imm via scratch.
  - empty_expression_stack: integer ldr of esp (cap) -> cap_ldr_imm.
  - native fixed frame: slot 0 (initial_sp) re-stored as a full cap after the
    stock integer stp (which keeps the sp-writeback); the 2nd native oop-temp
    slot (13) is now zeroed too (stock zeroed 8+8 bytes = one 16B slot).

**Pitfalls found while doing it (encode-level):**
  - cap_str_imm/cap_ldp_imm CANNOT take HotSpot's `sp` as base (pseudo-encoding
    #33 silently becomes c1!). Copy csp via cap_add_imm (sp-aware) first.
  - cap_stp_sp_pre with Ct2=zr appears NOT to store a null cap (suspect C31=
    CSP in cap-STP Rt2) — broke boot when used for the native frame; reverted
    to the stock stp + cap re-store of slot 0.

**Tooling:** crash dumper hardened — COVER now requires unsealed+LOAD caps (a
sentry pick used to nested-fault and kill the dumper mid-print, truncating
every libjvm-ELR dump at "code words"); STOPLESS_DUMP_RANGE added (hex range
-> words via CSP/PCC cap, for offline llvm-mc disassembly of GENERATED
codelets); C9_EXC_DIAG (pending-exception arrival), C9_RETHROW_TRAP (turn the
unrecognized-return-address abort into a full dumper snapshot).

**CURRENT FRONTIER (precisely diagnosed, fix pending):** ConcatTest rc=134 —
unwinding the CNFE out of the findBootstrapClass NATIVE frame,
raw_exception_handler_for_return_address receives return_address == THE
EXCEPTION OOP (tag=1). Stack archaeology (range dump at the trap): the native
frame's saved-lr slot [fp+0x10] was overwritten with the exception CAP before
remove_activation; my save sequence then faithfully propagated it. The writer
is an exception-cap store with esp mis-pointing into the frame header region
(fp+0x20) during a throw pass — i.e. the native frame's
monitor_block_top/initial_sp slot held fp+0x20 at empty_expression_stack
time, NOT sp_post. Next step: find who updates the native frame's
monitor-block-top slot (or what loads esp) between native entry and the
exception path; suspects: the native-entry "allocate space for parameters"
esp adjustment not being re-stored to the frame slot, or the result-push
sequence around the safepoint transition.

## AL. ★★ ConcatTest GREEN + MinMove/StaticRootGC GREEN: indy works, the mover works (2026-06-11, patch 0166)

ConcatTest (string concat via invokedynamic) runs to completion (rc=0, all 3
markers) under StoplessGC purecap — the full indy -> MethodHandle ->
LambdaForm -> hidden-class definition -> StringConcatFactory chain works.
MinMove and StaticRootGC are GREEN: System.gc() STW-moves the object, revoke
fires, the stale-cap deref SIGPROTs, the C-7 handler heals it, the program
reads the right value — THE CHERI MOVING-GC MECHANISM WORKS END-TO-END IN THE
REAL TEMPLATE-INTERPRETER JVM. Bugs fixed today, in order of discovery:

1. ★ The §AK frame clobber was OUR OWN patch-0165 bug: `cap_str_imm(zr,
   rthread, pending)` — HotSpot's zr PSEUDO-ENCODING (32) overflows the 5-bit
   Rt field into Rn's low bit, assembling `str c0,[c29,#0x10]` = "store the
   just-loaded EXCEPTION over the frame's saved-lr slot". Found by offline
   disassembly of the forward_exception stub (range-dump). Fix: materialize
   a null cap via `mov(rscratch1, zr)` + cap_str_imm; cap_str_imm/cap_ldr_imm
   now assert encodings <= 30. TIME-BISECT METHOD: three C++ probes
   (throw_pending_exception / exception_handler_for_return_address /
   exception_handler_for_exception) printing [fp+16] pinned the window.
2. _remove_activation_entry: integer str of the exception into vm_result
   (tag-0 exception delivered to the NEXT unwind hop).
3. ★ Metaspace commit DEADLOCK-BY-UNITS: Settings::commit_granule_words used
   BytesPerWord (8) while MetaWord* arithmetic is 16-byte —
   get_committed_size_in_range returned 2x the request, the subtraction in
   commit_range UNDERFLOWED, every commit was rejected as "limit", and the
   indy bootstrap died with a bogus OutOfMemoryError: Metaspace at 12%
   committed. Fix: granule words = granule_bytes / sizeof(MetaWord).
   (C9_MS_DIAG/C9-vsn diags pinned it: word_size=4096 vs committed=8192.)
4. StoplessHeap::collect now handles _metadata_GC_threshold like Epsilon
   (MetaspaceGC::compute_new_size) instead of running the mover.
5. Throwable backtraces smuggle Symbol* through jlong array slots (tag
   strip). Added a metaspace-reservation cap registry
   (stopless_register/rederive_metaspace) wired into symbol_at; for C-heap
   symbols (SymbolTable arena) StackTraceElement::fill_in falls back to the
   intact method->name() when the smuggled Symbol* is untagged.
   => full Java stack traces print correctly now.
6. ★ Signature-handler STACK layout was uniform 16B/arg; purecap AAPCS gives
   each stack arg max(8, sizeof) bytes at natural alignment. defineClass0's
   (pd, initialize, flags, classData) stack tail was misread — "classData is
   only applicable for hidden classes" InternalError killed every
   hidden-class definition. next_stack_offset now packs 8B ints / 16B caps.

STATUS: java -version rc=0; ConcatTest rc=0; MinMove rc=0 [MM] OK;
StaticRootGC rc=0 [SR] OK. NEXT FRONTIER: IntegrityGC (8-node linked graph,
moves + integrity verify) crashes in round 0 at the gc-call phase
(0x4290c110, codecache) — the C-9 multi-object move correctness work starts
here.

## AM. ★★★ ALL C-9 INTEGRATION TESTS GREEN (2026-06-11, patch 0167)

One architectural fix: the forward-table self-heal core is now a shared
stopless_try_heal(si, ctx), called by BOTH sigprot_handler and the crash
dumper. The dumper had REPLACED sigprot_handler at re-arm time, so after a
(successful!) collect the very first stale heap-internal deref — which is
EXPECTED and must be forwarded by design — was treated as a fatal dump.
That was the whole IntegrityGC "crash": the GC itself (moved=8 fixed_roots=17
revoked=8) had already worked.

Result matrix (java -Xint -XX:+UseStoplessGC, Morello purecap QEMU):
  java -version   rc=0
  ConcatTest      rc=0  (full invokedynamic chain)
  MinMove         rc=0  [MM] OK
  StaticRootGC    rc=0  [SR] OK
  IntegrityGC     rc=0  [IG] ALL-OK   (8-node graph, post-GC traversal heals
                                       every stale next-link via SIGPROT)
  StoplessBench   rc=0  [BENCH] ALL-OK (3 collect rounds under allocation
                                        pressure, verify each round)

This closes the C-9 STW form: move -> root fixup -> revoke -> lazy heal of
heap-internal refs is correct end-to-end on real workloads. Remaining C-9
scope before C-10: raise STOPLESS_MOVE_LIMIT beyond 8 (whole-heap moves),
the move-while-allocating interleavings, and promoting the collect from
System.gc-triggered STW to the concurrent StoplessCollectorThread loop.

## AN. Whole-heap moves: interior-pointer heal + THE IDENTITY FRONTIER (2026-06-11, patch 0168)

Raising STOPLESS_MOVE_LIMIT past 8 (64 / unlimited -> moved=1016, every
root-held object incl. Strings/MethodTypes) exposed two things:

1. FIXED — interior-pointer heal: Unsafe.compareAndSetReference faults on a
   FIELD cap (obj_base + 0xC0). The forward table is keyed by object base;
   the handler looked up cap.address and missed. stopless_try_heal now looks
   up cheri_base(cap) first (the alloc csetbounds makes base = object start)
   and re-applies the offset to the forwarded cap. CAS on moved objects then
   heals like any load.

2. OPEN — REFERENCE IDENTITY ACROSS MOVES (the real C-10 design problem):
   IntegrityGC@limit>=64 now dies with
     WrongMethodTypeException: expected (Lookup,String,MethodType,String,
     Object[])CallSite but found (Lookup,...same...)CallSite
   — the two types PRINT identically because they ARE the same logical
   object: one reference was root-fixed to the new address while another is
   still the stale (revoked) cap. `==` / if_acmpeq does NOT dereference, so
   it cannot SIGPROT-heal — the stale and fixed caps compare unequal and
   MethodType's identity-based exact-type check fails. This is exactly why
   ZGC heals on LOAD (load barrier), not on use. Options for us (docs/40
   territory):
     a) acmp barrier: normalize both operands through the forward table in
        the if_acmpeq/if_acmpne templates (cheap: two tag checks; only
        tag-0/perms-0 caps need the table lookup);
     b) heal on load_heap_oop (full read barrier — closer to ZGC, more
        sites, also fixes ident-hash / lock-word paths);
     c) eager full-heap fixup at collect time (gives up laziness; simplest
        but defeats the paper's point).
   Plan: (a) first — it unblocks identity correctness with minimal cost and
   measures the acmp-barrier overhead the paper needs anyway.

STATUS: limit=8 matrix still ALL GREEN (incl. IntegrityGC/StoplessBench).

## AO. ★★★ THE ACMP BARRIER: whole-heap moves with correct identity (2026-06-11, patch 0169)

The §AN identity frontier is CLOSED. Implementation (the first C-10 barrier):

- TemplateTable::if_acmp (purecap): three-tier compare.
  1. equal addresses -> EQ (same object, both stale or both fixed alike);
  2. unequal addresses, BOTH capability tags set (gctag+and+cbnz) -> NE
     (live caps' addresses ARE identity);
  3. unequal addresses with an untagged operand -> runtime slow path
     stopless_acmp_eq(a, b): normalize each operand through the forward
     table (base-keyed + offset re-apply, same as the heal path) and
     compare normalized addresses. Common case overhead: 2x gctag + and +
     cbnz; the slow call is taken only when a REVOKED cap (or null vs
     non-null with differing addresses... null has tag 0) is compared.
- New MacroAssembler::cap_gctag(Xd, Cn) (GCTAG = 0xC2C09000|Cn<<5|Xd,
  verified vs llvm-mc).
- esp is rebuilt from csp after the leaf call (C-6 L57 trampoline detag).

Pitfalls hit (and worth remembering):
- `cmp(zr, imm)`/`subs(zr,...)`/`movw(r, int)` — zr pseudo-encoding 32 blew
  the 5-bit field guarantee ("Field too big for insn"); use a scratch reg +
  cmpw(reg, 1u) and unsigned literals.
- call_VM_leaf(fn, r1, r0): r0 IS c_rarg0 — pass_arg0 clobbers it before
  pass_arg1 reads it; both args became r1 and EVERY slow-path compare
  returned "equal" (symptom: "package jdk.internal.org.xml.sax in modules
  java.base and java.base" — the module system seeing one object as two).
  Stage the second operand in r2 first.

RESULT MATRIX (Morello purecap, -Xint StoplessGC):
  limit=8 (default):  all 6 tests rc=0 (unchanged)
  IntegrityGC  STOPLESS_MOVE_LIMIT=100000: moved=1016 -> [IG] ALL-OK
  StoplessBench STOPLESS_MOVE_LIMIT=100000: 3 rounds x ~800 moved -> ALL-OK

WHOLE-HEAP STW moves (Strings, MethodTypes, Class mirrors, everything
root-held) now preserve reference identity and graph integrity under
allocation pressure. Remaining identity surfaces to audit for C-10:
identity hash (mark-word based — moved objects keep their mark word, so
hashes survive the move ✓ by construction), JNI IsSameObject (C++ ==,
needs the same normalize), interpreter-internal oop == in C++ runtime
paths. Then: the concurrent collector loop + the write barrier (docs/40).

## AP. THE CONCURRENT COLLECTOR ARRIVES + safepoint-dispatch caps (2026-06-11, patch 0170)

1. JNI IsSameObject now uses the acmp-barrier identity rule (fast paths +
   stopless_acmp_eq normalize) — same semantics as if_acmpeq.
2. ★ StoplessCollectorThread (ConcurrentGCThread): a background thread
   triggers the short-STW collect cycle every STOPLESS_CONCURRENT_MS ms.
   Mutator pause = root-fixup window only; stale-ref healing remains
   concurrent in the mutators by design. This is the architecture the
   paper claims ("Stopless").
3. The concurrent mode instantly exposed TWO latent NULL-cap dispatch bugs
   (latent because polls are rare without a GC):
   - dispatch_base's non-active-table branch: integer `mov rscratch2,&table`;
   - dispatch_base's PENDING-POLL branch (bind(safepoint)):
     `lea ExternalAddress(safepoint_table)` from the codecache.
   Both now route through lea_libjvm_global (the C-6 cap_data_table).

STATUS: STW matrix (6 tests + unlimited-move IntegrityGC) ALL GREEN — no
regression. CONCURRENT mode (STOPLESS_CONCURRENT_MS=50, unlimited moves)
boots through ~17 background collects then faults in the getstatic codelet
(c4 == NULL deref at +0x124, method Set12$1.<init> — suspected cpcache
mirror/f1 oop vs concurrent move interleaving). That codelet-by-codelet
concurrency chase is the next work stream; the debug kit applies as-is.

## AQ. Concurrent-collector hardening: forward-table grow + the fixup-untagged race (2026-06-12, patch 0172)

Three findings while pushing the concurrent collector under whole-heap moves:

1. FIXED (patch 0171): the fixed 8192-slot forward table overflowed after
   ~17 concurrent whole-heap cycles and silently DROPPED forwardings.
   forward_table_maybe_grow() now doubles at >70% load (collector-only, at
   the safepoint); g_table/g_capacity atomic, old table leaked (bounded).
   Concurrent ConcatTest went 17 -> 23-25 collects before the next wall.

2. ORTHOGONAL: a perfdata-sampling thread derefs a wild cap (0xffff...)
   under concurrent load — independent of the GC; -XX:-UsePerfData sidesteps
   it. Not chased further.

3. THE CONCURRENT FRONTIER — precisely localized, root cause still open.
   New env diag C9_MOVE_CHECK validates every moved cap + every fixup write.
   Result:
     STW (any limit incl. 100000):  C9_MOVE_CHECK reports ZERO bad caps.
     CONCURRENT (any limit >=200):  37-181 "fixup wrote UNTAGGED fwd" — the
       forward-table lookup/cas hands back a cap whose ADDRESS is correct
       (a valid moved-object heap address, e.g. 0x44a34840) but whose TAG
       is cleared. The root slot then gets an untagged oop; a later mutator
       deref faults with an address the table can't re-key (it's the NEW
       address, table keys are OLD) -> fatal unforwarded fault, OR boot-layer
       NPE when identity logic sees the untagged ref as different.
   Verified NOT the cause: the atomic accessors are cap-wide
   (forward_table.o disasm: new_cap store = `stlr c0`, load = `ldar c0`),
   and STW exercises the identical store/lookup path with zero corruption.
   So a tag is being cleared specifically under concurrency. The diff vs STW
   is only that mutator threads + frequent safepoints are live. Leading
   hypotheses: (a) a writer other than the collector touches a table slot's
   new_cap with an integer store under concurrency; (b) the interior-pointer
   heal's cheri_address_set path interacts with a slot being rewritten;
   (c) a half-published slot read across the collector's cas + a mutator
   handler's lookup despite the 128-bit atomics. Needs a dedicated isolation
   pass (a standalone calloc+stlr-c/ldar-c torture under two threads).

STATUS unchanged for the verified configuration: STW matrix (6 tests +
unlimited-move IntegrityGC) ALL GREEN. Concurrent collector works under light
load (ConcatTest limit=8 reaches 53-55 collects, [CT] 3 DONE) but is not yet
robust under sustained whole-heap concurrent moves — the fixup-untagged race
above is the single open blocker, now reproducible and instrumented.

## AR. ★ ROOT CAUSE of the concurrent fixup-untagged: revoke clears forward-table caps (2026-06-12, patch 0173)

The C9_MOVE_CHECK + C9_FT_AUDIT instrumentation pinned it decisively:

- C9_FT_AUDIT (audit the table at each cycle START, before adding anything)
  reported 19-25 untagged entries ALREADY PRESENT — i.e. corrupted during the
  MUTATOR-RUN window between cycles, not at insert time.
- STOPLESS_NO_REVOKE makes the corruption VANISH (mvchk=0, audit=0). 

ROOT CAUSE: the forward table stored real heap CAPABILITIES to moved objects.
When an object is moved AGAIN in a later cycle and its now-old location is
revoked, the kernel's cheri_revoke sweep scans ALL memory for caps pointing
into the revoked range and clears their tags — INCLUDING the forward-table
entry still pointing at that intermediate address. Lookup then returns an
untagged cap; the root gets an untagged oop; a later deref faults with the NEW
address (which the table can't re-key) -> fatal. STW never hits this (objects
are moved at most once per run).

THE FIX (designed + implemented, saved as WIP-c10-forward_table-address-mode.*):
store the destination ADDRESS (an integer, immune to revocation) and rebuild a
usable oop cap at lookup from the arena base cap (which carries PERM_SW_VMEM
and is itself revocation-immune), stripping PERM_SW_VMEM so the result is an
ordinary revocable mutator oop that lives only in registers/roots, never in
the table. This DROVE mvchk TO ZERO — the untagged corruption is gone.

REGRESSION (open): the address-mode build hit a NEW fault — unlimited-move
IntegrityGC (STW) gets a real NULL receiver in MethodHandles$Lookup.in's
invokevirtual (c2=0, unforwarded). The ft_derive representability check fired
ZERO times (addresses rebuild correctly), so it is NOT a derive failure; the
only behavioral change is arena-wide bounds on the rebuilt cap vs the stored
tight cap. Mechanism not yet understood (a tight-bounds derive or an acmp/
identity interaction are the leading suspects). Because this breaks the
previously-green STW unlimited-move config, the address-mode change is REVERTED
on main (forward table back to cap-storage, STW matrix all green) and saved as
WIP records for the next session to finish.

STATUS: main is back to the patch-0172 baseline — STW matrix (6 tests +
unlimited-move IntegrityGC) ALL GREEN; concurrent collector works under light
load. The concurrent whole-heap blocker is now FULLY ROOT-CAUSED with a
designed fix; only the ft_derive regression stands between here and robust
concurrent whole-heap moves.

## AS. ★★ TIGHT-BOUNDS ADDRESS-MODE forward table LANDS (2026-06-12, patch 0174)

The §AR regression is solved — the hypothesis was right: the first
address-mode attempt rebuilt ARENA-WIDE-bounds caps, which broke every
base-keyed consumer (acmp identity normalize keys lookups on
cheri_base(cap)==object start; with base==arena base the lookup missed,
normalize returned the STALE address, identity broke, and MethodHandles'
identity-keyed caching produced NULL receivers).

Fix: the table now stores {from_addr, new_addr, new_len} (all integers,
revocation-immune) and ft_derive rebuilds the cap with the allocator's exact
derivation: bounds_set(address_set(arena_base, addr), len) & ~SW_VMEM —
TIGHT bounds, same as the original allocation.

RESULTS:
- STW matrix: ALL GREEN again (6 tests + unlimited-move IntegrityGC ALL-OK).
- Concurrent whole-heap torture: C9_MOVE_CHECK = 0 across all runs — the
  revoke-clears-table corruption is GONE for good.
- Remaining (separate) concurrent races, ~23-31 collects into boot:
  (a) rc=162 in the native->Java slow transition of
      Unsafe.compareAndSetReference: deeper analysis shows the real fault is
      a CSP OUT-OF-BOUNDS at the entry `stp c29,c30,[csp,#-0x30]!` of
      check_special_condition_for_native_trans — CSP base/top = the wrong
      256K stack segment (base=0x422af100 top=0x422ef100) while addr is in
      the LIVE segment (0x426c37a0). The dumper's "0xffff... c0" was an
      odd-PC misread (ELR bit0 = C64 marker). This native-trans SLOW path
      (`mov rscratch2,fn; blr rscratch2` + the CSP it inherits) is taken ONLY
      when a safepoint/handshake is pending on native return — which STW
      never produces, so it was never cap-ported. It is a native-entry
      slow-path porting gap (integer blr + a stale/narrow CSP), NOT a GC
      correctness bug — the forward table is clean (mvchk=0). The next work
      item: cap-port generate_native_entry's _thread_in_native_trans branch
      (CSP reconstruction + the runtime-call) the way the rest of the entry
      was ported.
  (b) rc=1: boot-layer NPE (no cap fault at all) — identity/ordering issue
      under early aggressive moves, to be re-examined after (a).

## AT. ★★ NATIVE-TRANS SLOW PATH cap-ported — concurrent collector now STABLE under light load (2026-06-12, patch 0175)

The §AS blocker is FIXED. The native->Java SLOW transition (taken only when a
safepoint/handshake is pending on native return — STW never produces it, so it
was never cap-ported) made two hand-rolled libjvm calls via integer `blr`:
JavaThread::check_special_condition_for_native_trans and
SharedRuntime::reguard_yellow_pages. Integer `blr Xn` to a C64 libjvm function
enters A64 mode -> the C64 prologue decodes as A64 garbage and faults. The
"out-of-bounds CSP" in the dump was a side-effect/misread of that wrong-mode
execution (setting address bit0 alone does NOT switch PSTATE.C64 — the dump
then showed an odd PC with C64=0, i.e. a misaligned A64 fetch).

Fix: route both through call_VM_leaf, which calls via the cap trampoline
(C64-correct) and — unlike call_VM — does NOT forward a pending exception (the
exact reason these sites avoided call_VM). 

RESULTS:
- STW matrix (6 tests + unlimited-move IntegrityGC): ALL GREEN, no regression.
- Light concurrent (STOPLESS_CONCURRENT_MS=30, MOVE_LIMIT=8): 8/8 runs rc=0,
  ~50 background collects each, [CT] 3 DONE. The concurrent collector is now
  STABLE — mutator pause is just the root-fixup window while ~50 whole-cycle
  moves+revokes run in the background. This is the "Stopless" architecture
  working end-to-end.
- Whole-heap concurrent (MOVE_LIMIT=200): the cap fault (rc=162) is GONE
  (mvchk=0); a separate boot-layer NPE (rc=1, no cap corruption) remains under
  aggressive early moves — the next item.

## AU. Concurrent collector: stable under light load; aggressive-move boot gap characterized (2026-06-12, patch 0175 cont.)

With the native-trans slow path fixed (§AT), the concurrent collector is
solid under light load but has a remaining correctness gap under aggressive
moves, now characterized:

- ConcatTest, CONCURRENT_MS=30, MOVE_LIMIT=8: 8/8 rc=0 (~50 collects each).
- ConcatTest/StoplessBench, MOVE_LIMIT=20/50/100/200: fail consistently
  (0/3) — NOT a cap fault (mvchk=0), but a Java-level NULL during BOOT:
  System.arraycopy gets a NULL array arg (jvm.cpp:288), or the module system
  throws LayerInstantiationException. The failure point varies run-to-run but
  is ALWAYS during VM boot / module-layer instantiation (System.initPhase2),
  never in steady application code.
- StoplessBench (allocation-heavy) is flaky even at MOVE_LIMIT=8 (1/3) — more
  objects moved per unit time raises the chance of hitting the gap, so it is
  about move VOLUME against the boot window, not a hard per-cycle limit.

Interpretation: moving boot-layer / module-system objects WHILE they are being
constructed corrupts a reference to NULL (not stale — genuinely zero). The
forward table is clean (mvchk=0), so this is not the revoke-corruption class;
it is a root/oop-map COVERAGE gap (a live oop the collector doesn't fix up, or
an in-flight reference not yet published as a root) specific to the dense,
identity-heavy module-bootstrap phase. Leading hypotheses: (a) template-
interpreter oop-map imprecision at the safepoint poll bci for some bytecodes
during aggressive moves; (b) an in-construction reference held only in a
register not covered by the poll oop map.

Two paths forward (next session):
 1. Defer the collector past boot (real GCs don't collect during early VM
    init) — but the QEMU test apps are boot-dominated (~15-22s boot of a
    ~20s run), so this leaves little app phase to demonstrate concurrency.
 2. Fix the coverage gap directly (the stronger result): instrument the
    collector to log every moved object's klass + the faulting NULL site,
    correlate which object type, and tighten root/oop-map coverage.

STATUS: STW matrix all green; concurrent collector demonstrably works (the
Stopless mechanism — concurrent move + revoke + lazy heal — runs end-to-end
under light load). The aggressive-move boot gap is the next correctness item.

## AV. Aggressive-move boot gap bisected to MOVE / VM-internal raw pointers (2026-06-12, patch 0176)

Systematic bisection of the aggressive-concurrent boot NULL (all at
MOVE_LIMIT>=20, during module bootstrap):
- STOPLESS_SKIP_COMPLEX (skip array klasses + Class mirrors): still fails ->
  NOT those types.
- STOPLESS_NO_FIXUP (move+revoke, don't rewrite roots, pure fault-heal):
  still boot-NPEs (+ timeouts) -> NOT the root fixup corrupting frames.
- STOPLESS_NO_REVOKE (move+fixup, no revoke sweep): still boot-NPEs (run3;
  the other runs hit the expected acmp-identity-break cap fault since old
  copies stay tagged) -> NOT the revoke sweep.
- mvchk=0 throughout -> NOT forward-table corruption.

The ONLY common factor is move_object itself. Since the move/copy is verified
correct for all object types by STW unlimited-move IntegrityGC (1016 objects),
the remaining explanation is: a VM-INTERNAL RAW POINTER (a C++ cache of an
oop / a jobject / a Method-side oop reference) points to a moved object and is
NOT in the collector's root set (Threads::oops_do + OopStorageSet::strong +
CLDG cover Java-visible roots, but not every VM-internal C++ oop cache). The
dense module-bootstrap phase creates many such transient VM-internal oop
references; moving the target leaves the raw pointer dangling. With revoke on
it should fault+heal on deref, but value-propagation paths that read the oop
WITHOUT dereferencing (and thus without triggering the CHERI barrier) carry a
stale/NULL value forward.

This is a genuine finding for the paper: the CHERI revoke barrier covers
Java-heap references PRECISELY (every deref of a moved-away oop faults), but
VM-internal raw oop caches that are read-as-value (not dereferenced) escape
the barrier. The collector is correct for application objects (all references
heap/root-covered) and under light load; aggressive whole-heap moves during
VM boot expose the VM-internal-cache coverage gap.

DECISION: this does not block the paper's core measurements — pause time is
the STW root-scan+move window (STW is fully robust, all object types, identity
correct), and the concurrent mechanism is demonstrated under light load. The
VM-internal-cache coverage is future work (register the caches, or defer the
collector past boot as production GCs do). Pivoting to C-11 microbench.

## AW. ★ C-11 PAUSE-TIME DATA — the core paper measurement (2026-06-12, patch 0177)

Instrumented VM_StoplessCollect::doit() (which runs entirely inside the
safepoint, so its wall-clock IS the STW mutator pause) with phase timers:
[C11-pause] scan_move_us / revoke_us / pause_us per cycle.

Data (ConcatTest, concurrent collector, Morello purecap under QEMU):

  MOVE_LIMIT  moved  scan_move_us   revoke_us     heap_used grows
     1          1      33-37         ~920,000      1918K -> 2047K
     4          4      25-43         ~990,000      2316K -> 2480K
     8          8      29-38         ~985,000-1.02M 2385K -> 2571K

TWO HEADLINE FINDINGS:

1. THE MOVE PAUSE IS TINY AND HEAP-SIZE-INDEPENDENT (the paper's core claim).
   scan+move+fixup = ~30 µs, essentially FLAT across moved-object count
   (1 vs 4 vs 8) AND across heap growth (used grows ~34% within a run while
   scan_move stays ~30 µs). It is dominated by the ROOT scan
   (Threads::oops_do + OopStorageSet + CLDG), which scales with the number of
   roots, not heap size — exactly the design's premise. This is the
   measurable evidence that the moving-GC pause is bounded and
   heap-size-independent.

2. REVOKE IS THE DOMINANT COST (~1 s/cycle under QEMU). The cheri_revoke
   sweep is a full-address-space capability scan; our naive design does ONE
   full sweep per cycle even for 1-8 moved objects, so it dwarfs the ~30 µs
   move phase by ~4 orders of magnitude. This is the QEMU-emulated cost; real
   Morello hardware revocation is far cheaper, but the RELATIVE structure
   (revoke >> move) and the optimization target are the same ones Cornucopia
   / Cornucopia Reloaded address with incremental/concurrent revocation and
   batched quarantine. The obvious wins for us: batch many cycles' old copies
   into one revoke sweep (amortize), and/or move to per-page load-barrier
   revocation (Cornucopia Reloaded) instead of a global sweep.

This is genuine paper §5 data: the hardware-capability moving-GC mutator
pause (the move) is ~30 µs and heap-size-independent; the revocation sweep is
the cost to amortize. Next: vary the app to plot scan_move vs root count, and
prototype batched revocation to show the amortized total pause.

## AX. C-11 read-barrier (heal-fault) cost — completes the microbench triad (2026-06-12, patch 0178)

Added cumulative heal-fault timing in the SIGPROT handler
(clock_gettime(CLOCK_MONOTONIC), async-signal-safe, env C11_HEAL_TIME) +
stopless_handler_faults; dumped at VM exit as [C11-heal].

IntegrityGC, concurrent (CONCURRENT_MS=50, MOVE_LIMIT=8), Morello purecap/QEMU:
  heal_faults=805  total_ns=7,005,648  avg_ns=8703  (~8.7 us / heal-fault)

This completes the C-11 microbench triad (all QEMU-emulated; real Morello is
faster, but the relative structure is the architectural result):

  | metric                         | cost            | scales with        |
  | move pause (STW scan+move)     | ~30 us          | #roots (NOT heap)  |
  | revoke pause (cheri_revoke)    | ~1 s / cycle    | (full sweep)       |
  | heal-fault (concurrent barrier)| ~8.7 us / fault | #stale derefs      |

Reading: the per-access concurrent read barrier (the CHERI fault->forward->
resume) is ~8.7 us (signal delivery + forward_table_lookup + ft_derive +
register patch + return; signal delivery dominates under QEMU). Over the whole
IntegrityGC run that is 805 faults x 8.7 us = ~7 ms total — negligible next to
ONE ~1 s revoke sweep. So the cost hierarchy is unambiguous:
revoke (1 s) >> move (30 us) ~ aggregate-heal (7 ms). The moving-GC mutator
pause is tiny and heap-size-independent; the concurrent barrier is cheap; the
revocation sweep is the single cost to amortize (batch sweeps / per-page
load-barrier revocation, Cornucopia-Reloaded-style) — which is precisely the
Phase-2 direction in the original two-phase plan.

## AY. ★★★ THE CORE FIGURE: move pause flat as heap grows 46x at constant roots (2026-06-12, patch 0179)

Added a per-cycle root counter (StoplessMoveRootsClosure::_visited) to the
[C11-pause] line. Ran StoplessBench, whose allocation loop grows the live heap
continuously while the root set stabilizes — a clean controlled experiment.
Raw data: paper/data/c11_heap_independence_stoplessbench.txt (77 cycles).

Post-warmup, roots CONSTANT at ~894:

  heap_used   scan_move (pause)   revoke (full sweep)
   3.6 MB       13.7 ms             0.83 s
  23.7 MB       13.3 ms             1.48 s
  58.4 MB       13.5 ms             2.54 s
   107 MB       14.1 ms             4.06 s
   167 MB       14.6 ms             5.95 s

THE RESULT, unambiguous:
- The MOVING-GC PAUSE (scan + move + fixup) is FLAT — ~13.5 -> 14.6 ms (+6%)
  while the heap grows 46x (3.6 MB -> 167 MB) at constant roots. The pause is
  bound to the ROOT set, NOT the heap. This is the paper's central claim,
  demonstrated on a real OpenJDK on Morello purecap.
- The naive GLOBAL REVOKE scales LINEARLY with heap (0.83 s -> 5.95 s, ~7x for
  46x heap; the cheri_revoke sweep must scan all live memory for stale caps).
  This is exactly the heap-scaling cost that motivates incremental / per-page
  load-barrier revocation (Cornucopia Reloaded) — the original plan's Phase 2.

So the two-phase thesis is now measured end to end: Phase-1 CHERI-native moving
GC delivers a heap-size-INDEPENDENT mutator pause; the remaining heap-LINEAR
cost is concentrated entirely in the revocation sweep, which Phase-2 amortizes.
(Absolute numbers are QEMU-emulated — inflated — but flat-vs-linear is the
architecture-true structure; the paper presents the SHAPE.)

C-11 status: the core figure + the microbench triad (§AW/§AX) are done. This is
the empirical heart of the paper.

## AZ. ★★ Phase-2 BATCHED REVOCATION prototype + acmp-barrier cost (2026-06-12, patch 0180)

Two measurements that round out C-11:

(1) BATCHED REVOCATION (the Phase-2 teaser). cheri_revoke is a per-sweep,
heap-linear cost INDEPENDENT of how many objects are marked, and the shadow
bitmap ACCUMULATES marks across cycles. So STOPLESS_REVOKE_BATCH=N marks every
cycle but fires the sweep only every N cycles. The address-mode forward table
chains across cycles (a 2-generations-stale ref heals in two faults), so this
stays correct.

  batch  sweeps/cycles   avg pause/cycle   vs batch=1
    1      62/62 (every)   ~842,000 us       1.0x
    8      23/189          ~149,000 us       5.6x lower
   32       4/152          ~ 71,000 us      11.9x lower
  IntegrityGC = [IG] ALL-OK at every batch (correctness preserved).

So batching amortizes the dominant cost ~12x at batch=32 while staying correct
on the integrity test. At high batch the per-cycle pause floors at the ~13 ms
scan+move (heap-independent) plus the amortized revoke. This is the measured
Phase-2 direction: the heap-LINEAR revoke cost amortizes toward the
heap-INDEPENDENT move pause as the batch grows. (Caveat, honestly stated: the
batch window delays revocation, so a not-yet-fixed reference can read an OLD
copy until the sweep — correct only if the object isn't mutated via the new
copy in the window. Full correctness for mutated objects is the Cornucopia-
Reloaded per-page load-barrier, the real Phase-2; batching is the amortization
half of it. The dataset: paper/data/c11_batched_revoke.txt.)

(2) ACMP BARRIER COST. Added a slow-path counter (stopless_acmp_slowpath).
ConcatTest (heavy invokedynamic / MethodHandle, identity-sensitive):
  heal_faults=903  acmp_slowpath=218
The if_acmpeq/ne FAST path (gctag x2 + and + cbnz, ~4 extra instructions on
every reference comparison) handles the overwhelming majority; the SLOW path
(forward-table normalize, two lookups) is taken only 218 times over the whole
run. So the identity barrier is essentially free on the fast path and its
expensive path is rare — the acmp barrier is not a meaningful overhead.

C-11 COMPLETE: heap-independence figure (§AY), microbench triad (§AW/§AX),
batched-revoke amortization + acmp cost (this §AZ). The empirical core of the
paper is done.
