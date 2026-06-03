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
