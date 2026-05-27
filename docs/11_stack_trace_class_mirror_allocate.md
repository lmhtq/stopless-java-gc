# Stack trace: SIGSEGV in memset during `InstanceMirrorKlass::allocate_instance`

**Date:** 2026-05-27
**Methodology:** core dump cap-reg + saved-LR walk (no working debugger)
**Status:** Stack trace recovered. Root cause hypothesis but not closed.

## Stack trace (top→bottom)

```
frame 0: libc.so.7 memset+0x16b               (PC at trap)
frame 1: MemAllocator::allocate()              libjvm+0x898c9c
         — at `blr c2` virtual dispatch to initialize(mem)
frame 2: InstanceMirrorKlass::allocate_instance(Klass*, JavaThread*)
                                                libjvm+0x6782b0
         instanceMirrorKlass.cpp:55
frame 3+: inline helpers (ThreadShadow::has_pending_exception,
         StubGenerator::generate_md5_implCompress — likely interpreter-stub
         frame remnants since we're in early bootstrap, not real callers)
```

## What this tells us

The crash is the **first allocation of a java.lang.Class mirror**.

`InstanceMirrorKlass::allocate_instance(Klass* k)` is the function that
creates the Java-visible `Class` object for a freshly-defined Klass. It is
called from bootstrap class initialisation, exactly at the point where
post-`java.lang.Class`-load needs to materialise mirror objects for the
13 already-loaded bootclasses (Object, Serializable, ..., Class itself).

```cpp
instanceOop InstanceMirrorKlass::allocate_instance(Klass* k, TRAPS) {
  int size = instance_size(k);  // = align_object_size(size_helper() + static_field_size())
  return (instanceOop)Universe::heap()->class_allocate(this, size, THREAD);
}
```

`class_allocate` → `ClassAllocator(klass, size, T)` → `allocator.allocate()` →
`mem_allocate` (gets heap mem) → `initialize(mem)` (the blr c2 virtual).

`ClassAllocator::initialize`:
```cpp
oop ClassAllocator::initialize(HeapWord* mem) const {
  ...
  mem_clear(mem);           // ← memset chain crashes here
  java_lang_Class::set_oop_size(mem, (int)_word_size);
  return finish(mem);
}
```

`mem_clear`:
```cpp
const size_t hs = oopDesc::header_size();
oopDesc::set_klass_gap(mem, 0);
Copy::fill_to_aligned_words(mem + hs, _word_size - hs);
```

## Hypothesis

The dst at trap (`c0 = 0x42624630`) is on the **thread stack**, not the heap.
This contradicts `mem_clear(mem)` where `mem` is heap-allocated. So either:

1. memset is being called from a code path I haven't traced — e.g., the
   `Allocation allocation(*this, &obj)` constructor doing a stack zero-fill
   (compiler-generated), or
2. The blr-c2 indirect dispatch landed in a different function (not
   ClassAllocator::initialize) — possibly an interposer or wrapper that
   does its own memset
3. Some inlined operation in mem_clear / initialize uses memset for a
   stack-local buffer

The dst is in the active stack frame, not below it. memset is writing INTO
the current function's locals or saved frame area. Likely **a 0-init of an
auto Allocation/PreserveObj struct that has cap-typed members whose size
the compiler computed wrong**.

## Verified relevant code paths

| File:line | Code |
|---|---|
| memAllocator.cpp:43 | `class Allocation: StackObj` with 3 cap members + 3 bool + 1 size_t |
| memAllocator.cpp:90 | `class PreserveObj: StackObj` |
| memAllocator.cpp:393 | `ClassAllocator::initialize` |
| memAllocator.cpp:353 | `mem_clear` calls `Copy::fill_to_aligned_words` |
| copy_aarch64.hpp:31 | `pd_fill_to_words` — already patched 0064 |

## Verified register state at trap

| Reg | Value | Interpretation |
|---|---|---|
| PC  | 0x40504f80 | libc memset+0x16b |
| c0  | 0x42624630 | memset dst (stack addr) |
| c1  | 0xcafebabe | memset val (=0xbe) — `badHeapWordVal` magic |
| c30 | (clobbered by signal handler) | (not useful) |
| stack[SP+0x10] | 0x41e98c9c | saved LR → MemAllocator::allocate +0x100 |

The `0xcafebabe` value strongly suggests this memset is filling with HotSpot's
"bad value" pattern, used in DEBUG builds via `ZapResourceArea` /
`ResourceArea::Chunk::chop`. So this might be **a debug-zap memset, not a
production mem_clear**.

## Next investigation

1. Search for `memset(.*, 0xcafebabe & 0xff, ...)` or `Copy::fill_to_bytes(*, badHeapWordVal, *)` in HotSpot. The 0xbe byte value narrows the call site dramatically.
2. Check if ResourceArea or Arena allocates a temp chunk during mirror creation.
3. Look at `ScopeMark`, `ResourceMark`, `HandleMark` chunk lifecycle.

## Recovered techniques

- Walking AArch64 frame chain via saved FP/LR at SP+0/SP+0x10
- Extracting libjvm.so base from `procstat -v` of a live JVM
- Mapping runtime PC offsets to source via llvm-addr2line
- Reading NT_PRSTATUS + cap register section from FreeBSD core dump
