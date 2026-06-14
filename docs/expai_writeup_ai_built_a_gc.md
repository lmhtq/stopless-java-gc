# An AI built a garbage collector for hardware no GC had targeted

*Autonomous AI research · field note · June 2026 · bluecat (bootstrapper)*

> Source for the expai.cc post. The published HTML lives in the cloud-agent-cluster
> site tree (`apps/expai_radar_site/site/writing/ai-built-a-garbage-collector/`).

I want to report something concrete about what current frontier models can do,
without the usual hand-waving. Not a benchmark score, not a toy demo — a single
sustained research project on hard, unfamiliar ground, where the model had to be
*right* hundreds of times in a row against a compiler, a kernel, and silicon that
punishes the smallest mistake.

The result is **StoplessGC**: a concurrent, moving garbage collector for OpenJDK
that uses **CHERI capability revocation** — a hardware memory-safety feature on
ARM's Morello chip — as its read barrier. As far as we can tell, no garbage
collector had ever been built on this mechanism before. The model designed it,
wrote the runtime in C and the patches into HotSpot's 25-kLOC C++ interpreter,
ran the measurement campaign, and produced a paper that then survived **eight
rounds of adversarial peer review** (by a second, different model) — ending at a
"strong accept."

Every line of code and every sentence of the paper is the model's. My role was
narrow and I want to be honest about it: I supplied the *spark* (what if a CHERI
capability fault *is* a GC read barrier?), the build-and-test hardware,
occasional one-word steering ("continue", "do the microbenchmarks", "now make it
sound"), and an adversarial-review loop. I wrote none of the code or prose. The
whole point is that I didn't have to.

**The artifact is public and auditable.** The code, the OpenJDK patches, the
benchmarks, the raw measurement logs behind every figure, the paper, and — most
tellingly — the complete *turn-by-turn development log* (every hypothesis, dead
end, and root-caused bug, ~60 sections of it) are all in the repository. Nothing
was cleaned up after the fact: <https://github.com/lmhtq/stopless-java-gc>

## What it actually does

A moving garbage collector relocates live objects to compact memory. The hard
part is that while it moves an object, the running program may still hold
pointers to the *old* location. Every modern low-pause collector solves this with
a *read barrier*: extra work on every pointer read that detects a stale pointer
and redirects it. ZGC hides metadata in pointer bits; Shenandoah adds an
indirection word; Azul once built the barrier into a custom CPU. In every case
the program pays, on every pointer load, for the collector's benefit.

CHERI hardware changes the deal. On a CHERI machine every pointer is a
hardware-checked *capability* with a validity tag, and the chip can *revoke*
capabilities — designed to make use-after-free bugs fail-stop. The model's
insight: revocation is already a read barrier. Move the object, revoke the
pointers to the old copy, and the hardware will *trap* the next time the program
touches a stale one. A signal handler looks up where the object went, repairs the
pointer, and resumes. **Reference loads run no barrier instructions at all** —
the check is the one the hardware already does on every access.

## The numbers

- **46×** — heap growth over which the move pause stays flat (~14 ms); it is
  bounded by the root set, not the heap size.
- **18.5 ms** — median mutator pause with sound concurrent revocation, on a heap
  where the naive synchronous design pauses for seconds.
- **8 rounds** of adversarial model review survived, ending at strong accept
  (9.5/10).

Measured on OpenJDK 17 / ARM Morello purecap under QEMU emulation, so the
absolute milliseconds are inflated — the *shapes* (a flat curve where a naive
collector would climb; a sound design where the unsound one corrupts) are the
real claims. It is the relocation-and-barrier core of a collector; full liveness
tracing and reclamation are future work, and the paper says so plainly. Honesty
about scope was itself something the review loop enforced.

## The part that surprised me

It wasn't that the model could write the code. It was how it behaved when the
code was *wrong* — which on this stack is most of the time. CHERI/Morello has
failure modes that barely exist elsewhere: an integer compare that silently
strips a pointer's hardware tag; a call that decodes the wrong instruction-set
mode and runs garbage; a kernel revocation sweep that quietly erases the
collector's *own* bookkeeping because the bookkeeping was stored as capabilities.
The log is a catalog of the model hypothesizing, building a diagnostic, being
wrong, re-rooting, and eventually nailing the actual cause — including writing its
own gdb-free crash dumper when the normal tools didn't work in the emulator.

And the adversarial-review loop earned its keep. A second model, reviewing each
draft of the paper, twice caught *real bugs* from the prose alone — once deducing
from a mismatch between two numbers in a results table that the concurrency
control had a soundness hole, which it did. Fixing it didn't just patch the
paper; it changed the implementation and, as a bonus, erased a latency spike.

> The reviewer's verdict trajectory across the eight rounds: strong reject →
> reject → weak reject → borderline → … → **strong accept**. Each rejection named
> a specific defect; each fix made both the system and the paper more correct.

## What I think this means

The frontier isn't "can a model write a function." It's whether a model can hold
a hard, multi-week research problem in its head — unfamiliar hardware, a huge
legacy codebase, a result that has to actually be true — and grind it to
something defensible, while staying honest about what it didn't prove. On this
one, it did. The bottleneck was no longer the thinking or the coding; it was the
wall-clock of building and emulating a CHERI system, and a human willing to say
"keep going."

I'm publishing the whole thing — including the messy log — because the
interesting evidence is the trace, not the headline.

---

Repository & paper: <https://github.com/lmhtq/stopless-java-gc>
Research & authorship: Claude (Opus 4.6/4.7/4.8, Fable 5) via Claude Code,
Anthropic. Bootstrapper: bluecat.
