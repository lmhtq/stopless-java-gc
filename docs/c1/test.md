# Phase C-1 — test plan and results

**Status:** test plan; awaits C-2 to be testable (C-1 alone doesn't build)
**Date:** 2026-05-27

## What C-1 alone can be tested for

* **Syntactic validation of the patch** — `git apply --check` on the
  unmodified jdk17u tree should report no rejected hunks (this is
  trivial because the patch only adds new files).
* **Code review** — design.md + impl_notes.md should match the patch.

## What we cannot test until C-2

C-1 produces **dead code**: the new `src/hotspot/share/gc/stopless/`
directory is not yet referenced by any Makefile, so the OpenJDK build
ignores it. There is no way to exercise the new classes from C-1 alone.

That is by design — C-1 was scoped to "skeleton files only" so the
diff is small and self-contained.

## Tests deferred to C-2 (combined C-1+C-2 acceptance)

### T1: Build succeeds
```
$ scripts/apply_patches.sh          # applies 0001..0082
$ scripts/fast_iter.sh              # builds hotspot
```
Expected: hotspot-server-libs built; libjvm.so exists; no compile
errors mentioning `stoplessHeap`, `stoplessBarrierSet`, `Stopless`.

### T2: Flag is visible to the JVM
```
$ scripts/run.sh -XX:+PrintFlagsFinal -version | grep -i stopless
```
Expected output:
```
size_t StoplessArenaSize  = 268435456     {product}    {default}
uintx  StoplessGCCount    = 0             {diagnostic} {default}
bool   UseStoplessGC      = false         {product}    {default}
```

### T3: Selecting the GC is parsable
```
$ scripts/run.sh -XX:+UseStoplessGC -version
```
Expected: JVM starts, prints version banner, exits 0. The GC selection
machinery calls `StoplessArguments::create_heap()` → `new StoplessHeap()`
→ `initialize()` returns `JNI_OK`. No allocation happens during
`-version` so the nullptr-returning `mem_allocate` is never invoked.

### T4: Heap appears in JFR / VM print
```
$ scripts/run.sh -XX:+UseStoplessGC -Xlog:gc -version
```
Expected: gc-init log line mentions "Stopless Heap".

## Failure modes to prepare for

| Symptom | Likely cause | Phase |
|---|---|---|
| `error: 'Stopless' is not a member of 'CollectedHeap::Name'` | C-2 didn't extend the enum | fix in C-2 |
| `error: 'StoplessBarrierSet' is not a member of 'BarrierSet::Name'` | C-2 didn't extend the enum | fix in C-2 |
| Linker: undefined reference to `StoplessHeap::initialize()` | new dir not in JvmFeatures.gmk | fix in C-2 |
| `assert(heap->kind() == CollectedHeap::Stopless)` fires | -XX:+UseStoplessGC not actually selecting StoplessArguments | fix in C-2 (gcConfig.cpp) |
| SIGSEGV on `mem_allocate` returning nullptr | class loader trying real alloc | upgrade to C-4 |

## Sign-off

C-1 ships when:
- [x] patch file syntactically correct
- [x] design.md complete
- [x] impl_notes.md complete
- [ ] paired with C-2, T1-T4 above all pass

Mark complete only after combined C-1+C-2 acceptance.
