# Class injected fields: the cap-provenance problem at the Java/JVM seam

**Status:** root-cause documented, fix designed (patch 0066 pending)
**Date:** 2026-05-26
**Phase:** Phase 2 entry — moves from "make CHERI port build/boot" to "redesign the Java/JVM type seam for capabilities"

## Where we got stuck

After Phase 1.5 (16-byte HeapWord pivot, patches 0060B + 0064 + 0065), the
JVM advances from "6 classes loaded" to "13 classes loaded" before
faulting. The bootclass set that now loads cleanly:

```
java.lang.Object
java.io.Serializable
java.lang.Comparable
java.lang.CharSequence
java.lang.constant.Constable
java.lang.constant.ConstantDesc
java.lang.String                      ← was the previous blocker
java.lang.reflect.AnnotatedElement
java.lang.reflect.GenericDeclaration
java.lang.reflect.Type
java.lang.invoke.TypeDescriptor
java.lang.invoke.TypeDescriptor$OfField
java.lang.Class                       ← parses + defines OK,
                                        but post-load init faults
```

The fault is `SIGPROT PROT_CHERI_TAG` (CHERI hardware tag-zero trap),
fired during `java.lang.Class` post-load init. No `hs_err_pid`
backtrace beyond the first frame: the signal handler itself walks a
tag-zero cap and re-faults before printing.

## Root cause

`src/hotspot/share/classfile/javaClasses.hpp:254`:

```c
#define CLASS_INJECTED_FIELDS(macro)                                       \
  macro(java_lang_Class, klass,                  intptr_signature, false) \
  macro(java_lang_Class, array_klass,            intptr_signature, false) \
  ...
```

`intptr_signature` in `vmSymbols.hpp:347-348`:

```c
NOT_LP64(  do_alias(intptr_signature, int_signature)  )
LP64_ONLY( do_alias(intptr_signature, long_signature) )
```

On LP64, `intptr_signature == long_signature == "J"`. The field has
Java-level signature `J` (long).

When the Class mirror is materialised, HotSpot writes the native
`Klass*` capability into the `klass` field:

```c
// javaClasses.inline.hpp:254
Klass* k = ((Klass*)java_class->metadata_field(_klass_offset));
```

`metadata_field` loads `sizeof(Metadata*)` bytes from the offset
(via `HeapAccess<>::load_at`).

| Platform | sizeof(jlong) | sizeof(Metadata*) | Match? |
|---|---|---|---|
| Standard LP64 | 8 | 8 | ✅ |
| **CHERI purecap** | **8** | **16** | ❌ **8 vs 16** |

The field is laid out 8 bytes wide (Java `long`). The store/load
operate on 16 bytes (capability). Concretely:

- `set_klass(mirror, k)` writes 16 bytes starting at `_klass_offset`.
  The upper 8 bytes overwrite the *next* field
  (`_array_klass_offset`).
- `as_Klass(mirror)` reads 16 bytes starting at `_klass_offset`.
  The lower 8 bytes are the address; the upper 8 bytes are whatever
  the next field contains. The reconstructed "capability" has bogus
  metadata bits — in particular, the tag bit is zero.
- The first capability-aware load of this value (e.g., `k->name()`)
  hardware-traps with `PROT_CHERI_TAG`.

This is the canonical "Java `long` cannot represent a CHERI capability"
problem, surfacing at the *injected field* layer rather than at user
code.

## MOJO's solution (verified against public material)

The Manchester+THG team that built MOJO published their approach in
two locations:

- **Soteria Research blog post 8** ([soteriaresearch.org/news/post8](https://soteriaresearch.org/news/post8))
  — "Using JVM injected fields to store a capability"
- **MOJO blog: "Adapting Graal for Morello - Part 1"**
  ([mojo-jvm.org/news/graal-changes2.md](https://www.mojo-jvm.org/news/graal-changes2.md))
  — references the Soteria article for the underlying JVM mechanism

Their fix is to **reuse HotSpot's existing `T_ADDRESS` BasicType** as a
first-class field layout type. HotSpot already defines:

```c
enum BasicType {
  ...
  T_OBJECT      = 12,
  T_ARRAY       = 13,
  T_VOID        = 14,
  T_ADDRESS     = 15,   // ← already exists, sized as intptr_t
  T_NARROWOOP   = 16,
  T_METADATA    = 17,
  T_NARROWKLASS = 18,
  T_CONFLICT    = 19,
  T_ILLEGAL     = 99
};
```

`T_ADDRESS` is documented as describing "internal references within
the JVM as if they were Java types in their own right." It is used
elsewhere in HotSpot but **not currently as a field layout type** —
`classFileParser.cpp:1394` and `:1414` explicitly mark it
`BAD_ALLOCATION_TYPE`.

### MOJO's reported layout

For their `java.lang.MemoryAddress` class (a synthetic class for
holding native capabilities), MOJO reports:

```
Layout of class java/lang/MemoryAddress
 @0   32/-      RESERVED        ← object header: 16-byte markWord + 16-byte Klass*
 @32  "address"     T  16/16  REGULAR  ← T_ADDRESS field, 16 bytes
 @48  "rawAddress"  J  8/8   REGULAR  ← regular Java long
```

The signature character `T` corresponds to `T_ADDRESS`. The field
allocates 16 bytes on CHERI (sizeof(intptr_t)) and 8 bytes on
non-CHERI.

MOJO explicitly notes this "breaks the Java specification" because
standard Java grammar does not include a `T` signature. They limit
the leak to JVM-internal use only.

## Our patch plan (0066)

Files to modify (estimated ~200 LOC):

1. **`src/hotspot/share/utilities/globalDefinitions.hpp`**
   Add `BytesPerAddress = sizeof(intptr_t)` (16 on CHERI, 8 elsewhere).

2. **`src/hotspot/share/classfile/vmSymbols.hpp`**
   Add a new symbol `address_signature` that resolves to `"T"`. Keep
   `intptr_signature` alive (it's used in non-CHERI builds without
   issue) but switch the cap-typed injected fields to
   `address_signature`.

3. **`src/hotspot/share/classfile/classFileParser.cpp`**
   - Add allocation types `NONSTATIC_ADDRESS` and `STATIC_ADDRESS`
     equivalent in placement to `NONSTATIC_DOUBLE` / `STATIC_DOUBLE`
     on non-CHERI (both 8-byte), and to a new 16-byte slot on CHERI.
   - Replace the two `BAD_ALLOCATION_TYPE` entries for `T_ADDRESS`
     with the new allocation types.
   - Teach the layout pass that `T_ADDRESS` field sizes are
     `BytesPerAddress`.

4. **`src/hotspot/share/classfile/javaClasses.hpp`**
   Change the `CLASS_INJECTED_FIELDS` macro to use
   `address_signature` for `klass`, `array_klass`, and any other
   pointer-typed injected fields. Similar for
   `STRING_INJECTED_FIELDS`, `MODULE_INJECTED_FIELDS`,
   `STACKFRAMEINFO_INJECTED_FIELDS` etc. if they store caps.

5. **Conditional compilation:** all changes gated on
   `__CHERI_PURE_CAPABILITY__`. On non-CHERI the field layout is
   identical to upstream.

## Why this fits Phase 2 (and the paper)

This patch is the first *design-level* contribution of the project,
not a build/run fix. It is the explicit answer to the question MOJO
named "the perennial problem of CHERI" — reconciling existing
systems' assumption that pointers and addresses are interchangeable.

For the paper, this becomes part of the "mechanism" section:

- *Field-level cap provenance preservation*: extending the JVM field
  type system with a 16-byte first-class capability type, keeping
  tags out-of-band, separately from the Java `long` representation.
- *Compositional with the GC barrier work*: the cap-load barrier
  redesign (Phase 2 §3) operates on values that already have a
  consistent provenance — without this seam fixed, every Klass
  back-reference would need rematerialization.

## Open follow-ons

- **Object header layout (32 bytes on CHERI vs 16 on LP64).** MOJO's
  layout shows 32-byte reserved header. We have not verified our
  current `oopDesc` layout under purecap. If markWord stays 8 bytes
  but Klass* is 16 bytes, the header is 24 bytes; padding to 32 is
  conventional. Audit required.
- **Reflection / fieldDescriptor / Unsafe.** Java code accessing
  `T`-typed fields via reflection will need either an error or a
  cap-aware accessor. MOJO restricts T_ADDRESS visibility to JVM
  internals — we should do the same.
- **CDS interaction.** Archived mirrors store the klass field; if we
  change its byte width, CDS archives are not portable across the
  pivot. We have CDS disabled today so this is deferred.

## References

- soteriaresearch.org/news/post8 — primary technical reference
- mojo-jvm.org/news/graal-changes2.md — context + JavaKind.Address
- Glasgow CHERITech 2023 slides "OpenJDK on Morello" (Nisbet et al.)
- mojo-jvm.org/news/exactly-constrained-bounds (Feb 2024) — earlier
  context for 16-byte HeapWord choice
