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
