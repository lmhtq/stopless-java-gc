# C2 JIT on Morello — State of the Art

**Status**: R2 evidence-gathered against the public literature as of
2026-05. Findings here pre-fill what the spike day 6 would otherwise
have discovered by trial compilation.

**Sources surveyed**:
- *Pitfalls in VM Implementation on CHERI: Lessons from Porting
  CRuby* (Liu, Yamazaki, Ugawa). *The Art, Science, and Engineering
  of Programming*, vol. 11 no. 1, article 2 (Feb 2026). DOI
  `10.22152/programming-journal.org/2026/11/2`. PDF on arXiv:
  `2603.05645`.
- MOJO project public docs (`www.mojo-jvm.org/news/porting-jdk-gc-for-morello`).
- CRuby paper's own survey table covering JavaScriptCore [16],
  MicroPython [23], and Rust [17] ports.

## 1. Top-line finding

**No public, production-grade JIT compiler other than WebKit's
JavaScriptCore has been demonstrated working under CHERI purecap mode.**

CRuby (Feb 2026): "In our porting, we focus on the runtime system
and the interpreter, **excluding the JIT compiler**." This is a 2026
academic port of a relatively simple Ruby VM. They modified only 464
lines across 40 files in a 600k-LOC codebase — yet still chose to
skip the JIT.

MOJO (Manchester+THG): their public materials describe porting
*Epsilon, Serial, and G1*. C2 cap-awareness is not advertised as
working. The MOJO patches likely run at **C1 + interpreter** only.

JavaScriptCore: the lone JIT-on-CHERI precedent (Filaretti et al.,
referenced as [16] in the CRuby paper). Its FTL JIT is far simpler
than HotSpot C2 — fewer optimization passes, no SSA-based escape
analysis, no graph-based codegen.

## 2. Why C2 is harder than C1 / interpreter

The CRuby paper's category breakdown of "pitfalls" generalises to
any VM JIT:

| Pitfall (CRuby paper §4) | C1 / Interp impact | C2 impact |
|---|---|---|
| Capabilities seen as integers | Low (interpreter just dereferences) | **High** — C2 SSA values cross integer/pointer types freely |
| Padding bits of integer types | Low | **High** — register allocator assumes 64-bit slots |
| Compiler-introduced temporary capabilities | Low | **Medium** — C2 emits asm that the cheri-clang may try to fix up; meets aarch64-cap codegen |
| Modifying temporary caps | Low | **High** — C2 generates code that does exactly this for offset computations |
| Pointer arithmetic on non-cap types | Low | **High** — C2's array bounds elimination assumes integer pointer math |

The fundamental issue: **C2 emits machine code that itself has to
respect the CHERI purecap discipline.** Every `lea`-equivalent
becomes a cap-aware sequence; every memory access becomes a `LDR Cx`
or `STR Cx`; every offset computation must avoid producing a
sealed-cap fault.

## 3. R2 verdict

**R2 stays at High severity.** Evidence:

- No public port of OpenJDK C2 to Morello exists.
- The closest precedent (JavaScriptCore) is a much simpler JIT.
- MOJO has not advertised C2 cap-awareness in 18+ months of work.
- The CRuby team in 2026 still considered JIT out-of-scope for a
  workshop-grade port.

## 4. Mitigation paths

### 4.1 Phase 1 — run at C1 + interpreter

Demonstrably feasible (MOJO has done it for G1). Disable C2 in
the Phase 1 build by passing `-XX:-TieredCompilation` (or by
configuring `make` to drop C2 from the build entirely). Performance
numbers in the Phase 1 paper are then "C1-tier"; we frame this
honestly and use MOJO G1 (also C1-tier) as the apples-to-apples
baseline.

Build-system control: in `scripts/build_jdk.sh`, pass
`--with-jvm-features=-compiler2` to drop C2. We will add this flag
once we verify it's recognized; otherwise a small build-system patch
suffices (~30 LOC).

### 4.2 Phase 2 — C2 cap-awareness becomes the critical path

The Phase 2 headline claim ("≥3× per-load barrier cost reduction")
relies on the barrier being inlined by C2. If C2 doesn't work in
purecap mode, the barrier swap happens in C1-generated code, which:

1. Is itself slower than C2 by ~2–4× on hot paths.
2. Means our barrier delta is measured against a C1 baseline, not
   C2 — apples-to-apples is preserved but the absolute throughput
   numbers are weaker.

The honest framing: "On a JVM running C1 + interpreter, replacing
ZGC's software load barrier with a CHERI cap-load barrier reduces
per-load barrier cost by ≥3×."

### 4.3 Phase 1.5 — add C2 cap-awareness as a separate contribution

The CRuby paper §4.4–4.7 already enumerates the C2-level pitfalls
in detail. The actual engineering — modifying HotSpot's C2 backend
to emit cap-aware instructions — is a multi-month sub-project. It
deserves its own workshop paper.

Budget if undertaken: ~2–3 kLOC of HotSpot C2 patches,
predominantly in `src/hotspot/cpu/aarch64/c2_*.cpp` (touched by the
Morello CPU subdirectory we already plan to add).

## 5. Spike validation needed

On day 6 of the Phase 0 spike, after `bootstrap.sh` completes:

1. Build MOJO G1 with `-XX:+UseG1GC -XX:+TieredCompilation`. Confirm
   whether C2 compiles methods at all in purecap mode.
2. If C2 compiles: disassemble a hot method and check whether
   loads use `LDR Cx` (cap-aware) or `LDR Xn` (raw 64-bit, which
   would discard cap tag).
3. If C2 doesn't compile in purecap: confirm that C1 builds work
   end-to-end, document the failure mode of C2, file it as the
   Phase 1 path.

## 6. References

- CRuby paper, §3 (porting methodology), §4.4–4.7 (compiler-level
  pitfalls), Table 1 (survey of prior VM ports).
- MOJO project blog posts (no C2 reference found).
- `docs/04_risk_register.md §R2` — original risk statement.
