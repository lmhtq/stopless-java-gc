# StoplessGC — capability revocation as the read barrier of a moving GC

**A CHERI-native concurrent moving garbage collector for OpenJDK, designed,
built, debugged, and measured autonomously by AI.**

A moving garbage collector needs a *read barrier*: a check on every reference
load that catches references to objects the collector has relocated and
redirects them. ZGC does it with colored pointers, Shenandoah with a
forwarding word, Azul once did it in custom silicon. This project does it with
**CHERI capability revocation** — a hardware temporal-safety feature on ARM
Morello that makes the load–store unit *fault* on any use of a revoked
pointer. StoplessGC relocates an object, revokes the capabilities to the old
copy, and lets the hardware trap each surviving stale reference; a signal
handler forwards it through a table and resumes the load. **Reference loads
execute no barrier instructions** — the barrier is the capability tag check
the hardware already performs on every access.

The paper is **[`paper/stopless/`](paper/stopless/)** (*Trap, Forward,
Resume: Capability Revocation as the Read Barrier of a Moving Garbage
Collector on CHERI*).

## This is an autonomous-AI-research artifact

Every line of code, every debugging step, the entire measurement campaign, and
the paper were produced by **Anthropic's Claude models** (the Opus 4.6/4.7/4.8
line over the project, Fable 5 in the concurrent-collector and Phase-2 work),
running via **Claude Code**, across many hundreds of build–test–debug
iterations on a CHERI/Morello stack. A human, **Xiaohui Luo**, was the
*bootstrapper*: he supplied the research spark, the Morello/CheriBSD build and
test hardware, high-level steering, and an adversarial second-model review loop
— but wrote none of the code or prose.

The point of publishing this is to show, concretely and auditably, what current
models can do on a real systems-research problem: novel hardware no prior GC
has targeted, a 25-kLOC C++ runtime to bend (HotSpot), and a result that
survived eight rounds of adversarial review. The full turn-by-turn record —
every hypothesis, dead end, root-caused bug, and design law — is in
[`docs/41_c9_research_handler_fix_and_zero_boot_blocker.md`](docs/41_c9_research_handler_fix_and_zero_boot_blocker.md)
(~60 sections, §A–§BH). Nothing here is polished after the fact; the log is the
real trace.

## Results (OpenJDK 17 interpreter, ARM Morello purecap, under QEMU)

- **The relocation pause is independent of heap size.** With the root set
  held constant, the move pause stays ~13–15 ms while the live heap grows
  **46×** (3.6 → 163 MiB) — it is bounded by roots + bytes copied, not heap.
- **Sound concurrent revocation, implemented.** Using Morello's per-page
  capability-load (CLG) barrier, the collector opens a revocation epoch
  *inside* the pause (milliseconds) and runs the heap-linear sweep *off-pause*:
  median mutator pause **18.5 ms** (p90 25.7, full trace) on a heap growing to
  79 MiB where the synchronous design pauses for seconds.
- **The identity barrier is nearly free**, and forwarding metadata must be
  *integers* re-derived from revocation-exempt roots (storing capabilities is
  fatal — the sweep clears the collector's own table; a CHERI-specific design
  law the project root-caused the hard way).

All numbers trace to raw logs under [`paper/data/`](paper/data/), including
fault-injection runs of the fail-safe paths.

> Caveat, stated plainly: measured under QEMU system emulation, so absolute
> times are inflated — the **shapes** (flat vs linear, sound vs unsound) are
> the architectural claims, not the milliseconds. It is the *relocation and
> barrier substrate* of a collector; liveness, reclamation, and address reuse
> are future work, and the paper says so.

## Layout

```
src/cap_runtime/stopless_gc/   the CHERI-side runtime (C): arena allocator,
                               forwarding table, SIGPROT heal handler, revoke
patches/openjdk-jdk17/         in-place HotSpot patches (.record = git diff per
                               file) — interpreter, GC heap, template table
tests/integration/             ConcatTest, IntegrityGC, StoplessBench, ...
paper/stopless/                the paper (LaTeX), figures, plots, raw data
docs/41_*.md                   the autonomous development war-log (§A–§BH)
third_party/                   NOT committed — OpenJDK 17u + CheriBSD are
                               fetched/patched locally (see scripts/)
```

## Reproducing

The build needs a Morello/CheriBSD cross-toolchain, a purecap CheriBSD guest,
and OpenJDK 17u. The CHERI runtime builds standalone
(`cd src/cap_runtime/stopless_gc && make CROSS=1`); the HotSpot patches apply
to a jdk17u checkout. End-to-end this is a substantial environment to stand up
— the war-log documents it. The paper compiles with no hardware:
`cd paper/stopless && make` (uses the `texlive/texlive` Docker image).

## License

Apache-2.0 for the net-new runtime and tooling (see [`LICENSE`](LICENSE)).
The OpenJDK patches inherit GPLv2-with-Classpath-exception when applied to the
OpenJDK tree.

## Credit

Research and authorship: **Claude (Opus 4.6/4.7/4.8, Fable 5)** via Claude
Code, Anthropic. Bootstrapper: **Xiaohui Luo** (lmhtq1991@gmail.com). Part of
an experiment in autonomous AI research — see [expai.cc](https://expai.cc).
