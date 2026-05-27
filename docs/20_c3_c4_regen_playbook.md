# Playbook: regenerate C-3 (0083) and C-4 (0085) patches on hasee

**Why this exists:** patches 0083 and 0085 are still hand-authored
from the Mac session. They will not apply cleanly. Use this playbook
to regenerate them as canonical patches via the workflow that
already worked for 0080/0081.

**Time estimate:** 30-45 min focused work on hasee.

**Prerequisites:**
- On hasee, repo at commit ≥ `f7cf91e` (handoff doc commit)
- patch-state file shows 0080+0081 applied; 0083, 0085 NOT applied
- libjvm.so currently builds but `mem_allocate` returns nullptr

## Step 1 — Confirm starting state

```bash
ssh bc@hasee
cd ~/projs/stopless-java-gc
tail -6 third_party/openjdk-jdk17/.stopless/patch-state
# Expected:
#   ...APPLIED=0080-stopless-gc-skeleton.patch
#   ...APPLIED=0081-stopless-gc-feature-enable.patch

ls third_party/openjdk-jdk17/src/hotspot/share/gc/stopless/
# Expected: stoplessHeap.{hpp,cpp} + Arguments + BarrierSet + globals
# Should NOT yet have stoplessArena.{hpp,cpp} — those are from C-3
```

## Step 2 — Apply C-3 intent (StoplessArena C++ wrapper)

The intent (see `docs/c3/design.md`):
1. Create `src/hotspot/share/gc/stopless/stoplessArena.hpp` with the
   StoplessArena class declaration.
2. Create `stoplessArena.cpp` that wraps cap_runtime/stopless_gc/
   functions via `extern "C"` includes.
3. Modify `stoplessHeap.hpp` to forward-declare StoplessArena, replace
   `_arena_capacity`/`_arena_used` fields with `_arena*`, and turn
   `capacity()`/`used()`/`max_capacity()` from inline to out-of-line.
4. Modify `stoplessHeap.cpp` to create the arena in `initialize()`
   and consult it in `is_in()`, `capacity()`, `used()`, `print_on()`.

```bash
# Write the .hpp/.cpp content based on docs/c3/design.md.
# Use exact-string substitutions, not regex — see
# scripts/apply_stoplessgc_to_openjdk.py for the working template.

cd ~/projs/stopless-java-gc/third_party/openjdk-jdk17

# Create stoplessArena.hpp (full text in docs/c3/design.md §3.1)
cat > src/hotspot/share/gc/stopless/stoplessArena.hpp <<'HPP'
... (paste from docs/c3/design.md, with forward-declaration of
     stopless_arena struct, RAII class, capacity/used/contains/allocate/
     mark_for_revoke/revoke_sweep/print_on/verify methods) ...
HPP

# Create stoplessArena.cpp (full text in docs/c3/design.md, includes
# revoke.h and api.h via extern "C", calls stopless_arena_init/fini/
# stopless_mark_revoke_cap/stopless_revoke_now)
cat > src/hotspot/share/gc/stopless/stoplessArena.cpp <<'CPP'
... (paste from docs/c3/design.md §3 + impl_notes.md) ...
CPP

# Modify stoplessHeap.hpp — exact-string edits:
python3 <<'PY'
p = "src/hotspot/share/gc/stopless/stoplessHeap.hpp"
s = open(p).read()
# 1. Forward declare StoplessArena
s = s.replace(
    "class StoplessBarrierSet;",
    "class StoplessBarrierSet;\nclass StoplessArena;",
)
# 2. Replace fields
s = s.replace(
    "  size_t               _arena_capacity;\n  size_t               _arena_used;",
    "  StoplessArena*       _arena;       // owned, created in initialize()",
)
# 3. Out-of-line capacity/used/max_capacity
s = s.replace(
    "  size_t capacity() const          { return _arena_capacity; }",
    "  size_t capacity() const;",
)
s = s.replace(
    "  size_t used() const              { return _arena_used; }",
    "  size_t used() const;",
)
s = s.replace(
    "  size_t max_capacity() const      { return _arena_capacity; }",
    "  size_t max_capacity() const;",
)
open(p, "w").write(s)
PY

# Modify stoplessHeap.cpp similarly — see docs/c3/design.md.
```

**Validate**: Build hotspot. Expected: compiles + links. libjvm.so
size should change. Test:
```bash
cd build/jdk-morello && gmake CONF_CHECK=ignore hotspot-server-libs
```

**Regenerate patch:**
```bash
cd ~/projs/stopless-java-gc/third_party/openjdk-jdk17
git add src/hotspot/share/gc/stopless/stoplessArena.{hpp,cpp}
git diff --cached -- src/hotspot/share/gc/stopless/stoplessArena.{hpp,cpp} \
  > /tmp/0083-new-files.patch
git diff -- src/hotspot/share/gc/stopless/stoplessHeap.{hpp,cpp} \
  > /tmp/0083-mods.patch
cat /tmp/0083-new-files.patch /tmp/0083-mods.patch > /tmp/0083-regen.patch

# Prepend a Subject header:
cat > /tmp/0083-header <<'EOF'
From: lxh <lmhtq1991@gmail.com>
Date: <today>
Subject: [PATCH] Phase C-3: StoplessArena C++ wrapper over cap_runtime

Adds StoplessArena class wrapping cap_runtime/stopless_gc C API
(stopless_arena_init/fini, mark_for_revoke, revoke_now). Replaces
the placeholder _arena_capacity/_arena_used fields in StoplessHeap
with a real arena that mmap's CHERI-tagged memory and acquires the
shadow revocation bitmap.

Design: docs/c3/design.md

EOF
cat /tmp/0083-header /tmp/0083-regen.patch > \
    ~/projs/stopless-java-gc/patches/openjdk-jdk17/0083-stopless-arena-cpp-bridge.patch
```

## Step 3 — Apply C-4 intent (Allocator wire-up)

The intent (see `docs/c4/design.md`):
1. Modify `stoplessArena.cpp` to add `allocate()` that calls
   `stopless_alloc()` from cap_runtime + mirrors bump_offset.
2. Add `used()` to read C-side atomic + `reset()` method.
3. Modify `stoplessHeap.cpp::mem_allocate` to call
   `_arena->allocate(size * HeapWordSize)` and log OOM warning.

```bash
# Apply changes directly. See docs/c4/design.md §5-6 for the exact
# function bodies.

python3 <<'PY'
# Patch stoplessArena.cpp
p = "src/hotspot/share/gc/stopless/stoplessArena.cpp"
s = open(p).read()
# 1. Add allocator.h include
s = s.replace(
    '#include "revoke.h"\n#include "api.h"',
    '#include "revoke.h"\n#include "allocator.h"\n#include "api.h"',
)
# 2. Replace nullptr-returning allocate
s = s.replace(
    "void* StoplessArena::allocate(size_t bytes) {\n"
    "  // C-4 will fill this in (bump-pointer + csetbounds + perm strip).\n"
    "  // For C-3 we return nullptr so callers see \"no memory\" gracefully.\n"
    "  (void)bytes;\n"
    "  return nullptr;\n"
    "}",
    "void* StoplessArena::allocate(size_t bytes) {\n"
    "  if (_c == nullptr) return nullptr;\n"
    "  void* p = stopless_alloc(_c, bytes);\n"
    "  _bump_offset = stopless_arena_used(_c);\n"
    "  return p;\n"
    "}\n\n"
    "size_t StoplessArena::used() const {\n"
    "  if (_c == nullptr) return 0;\n"
    "  return stopless_arena_used(_c);\n"
    "}\n\n"
    "void StoplessArena::reset() {\n"
    "  if (_c == nullptr) return;\n"
    "  stopless_arena_reset(_c);\n"
    "  _bump_offset = 0;\n"
    "}",
)
open(p, "w").write(s)

# Patch stoplessHeap.cpp::mem_allocate
p = "src/hotspot/share/gc/stopless/stoplessHeap.cpp"
s = open(p).read()
# Replace the nullptr-returning stub body
# (look at current file to write the right anchor)
PY

# Build + test:
cd build/jdk-morello && gmake CONF_CHECK=ignore hotspot-server-libs

# Regenerate patch:
cd ~/projs/stopless-java-gc/third_party/openjdk-jdk17
git diff -- src/hotspot/share/gc/stopless/stoplessArena.cpp \
            src/hotspot/share/gc/stopless/stoplessHeap.cpp \
  > /tmp/0085-regen.patch
# prepend header similar to 0083
```

## Step 4 — Update patch-state

```bash
# Mark 0083 + 0085 as applied to make apply_patches.sh idempotent:
cat >> third_party/openjdk-jdk17/.stopless/patch-state <<EOF
BASE_SHA=eed263c8077e066a32d746fd188f2dee8c47b9ab
APPLIED=0083-stopless-arena-cpp-bridge.patch
BASE_SHA=eed263c8077e066a32d746fd188f2dee8c47b9ab
APPLIED=0085-stopless-arena-allocate-wire.patch
EOF
```

## Step 5 — Verify

```bash
bash scripts/c_phase_verify.sh
```

Expected progression vs current state:
- Stage A: still PASS (cap_runtime unchanged)
- Stage B: 0080+0081 still pass; 0083+0085 will fail dry-run because
  they expect 0080's files to exist (correct — dry-run is on pristine
  tree). Ignore those rejections.
- Stage C: build should succeed (gc/stopless/ compiles, including the
  Arena wrapper using cap_runtime symbols).
- Stage D: STILL FAILS — `java -XX:+UseStoplessGC -version` will
  proceed further but crash on shift=64 (C-6). C-5 + C-6 are paired.

## Step 6 — Commit + push

```bash
cd ~/projs/stopless-java-gc
git add patches/openjdk-jdk17/0083-stopless-arena-cpp-bridge.patch \
        patches/openjdk-jdk17/0085-stopless-arena-allocate-wire.patch
git commit -m "Phase C-3/C-4: canonical patches regenerated on hasee"
git push origin main
```

## What you must NOT skip

1. After editing OpenJDK files, ALWAYS build + verify before
   regenerating the patch. A patch derived from a broken state will
   propagate the breakage to next session.
2. Update `.stopless/patch-state` to match what's actually applied.
3. Push to gitee — the regen patches need to be available for any
   future session.

## Sanity-check: does it work?

After Step 5, on hasee:

```bash
ssh -p 10005 root@localhost \
  '/opt/jdk/bin/java -XX:+UnlockExperimentalVMOptions -XX:+UseStoplessGC \
                     -XX:-UseCompressedClassPointers -Xms32m -Xmx64m -version'
```

Expected output: still crashes (because C-6 not done yet), but the
crash should happen LATER than today's "In-address space security
exception" — specifically, after some class loading happens (which
means `mem_allocate` is actually returning memory, not nullptr).

If the crash signature changes to the shift=64 family (sbfm/ubfm
encoder failure), C-3+C-4 worked, and C-6 is the next blocker.
