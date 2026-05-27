#!/usr/bin/env bash
# apply_0081_intent_on_hasee.sh — make the 0081 edits directly to the
# openjdk-jdk17 tree on hasee, then 'git diff' to capture a clean
# patch with real line numbers.
#
# Run ON hasee from ~/projs/stopless-java-gc:
#   bash scripts/apply_0081_intent_on_hasee.sh
#
# Output: prints the diff to stdout. Redirect to a patch file.

set -euo pipefail

JDK_TREE="$HOME/projs/stopless-java-gc/third_party/openjdk-jdk17"
cd "${JDK_TREE}"

# 1. collectedHeap.hpp — enum Name += Stopless
python3 - <<'PY'
import re, sys
p = "src/hotspot/share/gc/shared/collectedHeap.hpp"
s = open(p).read()
new = re.sub(r"(    Shenandoah)\n(  \};)", r"\1,\n    Stopless\n\2", s, count=1)
if new == s:
    sys.exit("could not patch collectedHeap.hpp")
open(p, "w").write(new)
PY

# 2. barrierSet.hpp — enum Name += StoplessBarrierSet
python3 - <<'PY'
import re, sys
p = "src/hotspot/share/gc/shared/barrierSet.hpp"
s = open(p).read()
# find the enum Name { ... Unknown ... }; pattern
new = re.sub(r"(    ZBarrierSet,\n)(\s+Unknown)", r"\1    StoplessBarrierSet,\n\2", s, count=1)
if new == s:
    sys.exit("could not patch barrierSet.hpp")
open(p, "w").write(new)
PY

# 3. gc_globals.hpp — include stopless_globals + UseStoplessGC + STOPLESSGC_ONLY block
python3 - <<'PY'
import re, sys
p = "src/hotspot/share/gc/shared/gc_globals.hpp"
s = open(p).read()

# (a) After epsilon_globals.hpp include
new = s.replace(
    '#include "gc/epsilon/epsilon_globals.hpp"\n',
    '#include "gc/epsilon/epsilon_globals.hpp"\n'
    '#if INCLUDE_STOPLESSGC\n'
    '#include "gc/stopless/stopless_globals.hpp"\n'
    '#endif\n', 1)
if new == s: sys.exit("gc_globals.hpp: could not add include")
s = new

# (b) After UseShenandoahGC product line group, before UseZGC.
# Insert UseStoplessGC product flag.
new = re.sub(
    r"(  product\(bool, UseShenandoahGC, false,\s*\\\n"
    r'          "Use the Shenandoah garbage collector"\)\s*\\\n)'
    r"(\s+\\\n)"
    r"(  product\(bool, UseZGC,)",
    r'\1\2'
    r'  product(bool, UseStoplessGC, false,                                       \\\n'
    r'          "Use the Stopless (CHERI-accelerated) garbage collector")         \\\n'
    r'                                                                            \\\n'
    r'\3',
    s, count=1)
if new == s: sys.exit("gc_globals.hpp: could not add UseStoplessGC product")
open(p, "w").write(new)
PY

# 4. gcConfig.cpp
python3 - <<'PY'
import re, sys
p = "src/hotspot/share/gc/shared/gcConfig.cpp"
s = open(p).read()

# (a) include
new = s.replace(
    '#include "gc/epsilon/epsilonArguments.hpp"\n#endif\n',
    '#include "gc/epsilon/epsilonArguments.hpp"\n#endif\n'
    '#if INCLUDE_STOPLESSGC\n#include "gc/stopless/stoplessArguments.hpp"\n#endif\n', 1)
if new == s: sys.exit("gcConfig.cpp: include not added")
s = new

# (b) static StoplessArguments declaration after EpsilonArguments
new = s.replace(
    "EPSILONGC_ONLY(static EpsilonArguments epsilonArguments;)\n",
    "EPSILONGC_ONLY(static EpsilonArguments epsilonArguments;)\n"
    "STOPLESSGC_ONLY(static StoplessArguments stoplessArguments;)\n", 1)
if new == s: sys.exit("gcConfig.cpp: static decl not added")
s = new

# (c) SupportedGCs entry — after Epsilon, before Z (or wherever Z is)
# Look for EPSILONGC_ONLY_ARG line, insert Stopless after it.
new = re.sub(
    r'(EPSILONGC_ONLY_ARG\(SupportedGC\(UseEpsilonGC,[^\n]*\n)',
    r'\1  STOPLESSGC_ONLY_ARG(SupportedGC(UseStoplessGC,   CollectedHeap::Stopless,   stoplessArguments,   "stopless gc"))\n',
    s, count=1)
if new == s: sys.exit("gcConfig.cpp: SupportedGCs entry not added")
open(p, "w").write(new)
PY

# 5. arguments.cpp
python3 - <<'PY'
import re, sys
p = "src/hotspot/share/runtime/arguments.cpp"
s = open(p).read()
# Find UseEpsilonGC block, insert UseStoplessGC after it
new = re.sub(
    r'(  if \(UseEpsilonGC\) \{\n'
    r'    return EPSILONGC_ONLY_ARG\(JNI_OK\) NOT_EPSILONGC\(JNI_ERR\);\n'
    r'  \}\n)',
    r'\1'
    r'  if (UseStoplessGC) {\n'
    r'    return STOPLESSGC_ONLY_ARG(JNI_OK) NOT_STOPLESSGC(JNI_ERR);\n'
    r'  }\n',
    s, count=1)
if new == s: sys.exit("arguments.cpp: UseStoplessGC block not added")
open(p, "w").write(new)
PY

# 6. macros.hpp
python3 - <<'PY'
import re, sys
p = "src/hotspot/share/utilities/macros.hpp"
s = open(p).read()
# Insert STOPLESSGC block after EPSILONGC block
new = re.sub(
    r'(#ifndef INCLUDE_EPSILONGC.*?#endif // INCLUDE_EPSILONGC\n)',
    r'\1\n'
    r'#ifndef INCLUDE_STOPLESSGC\n'
    r'#define INCLUDE_STOPLESSGC 1\n'
    r'#endif // INCLUDE_STOPLESSGC\n'
    r'#if INCLUDE_STOPLESSGC\n'
    r'#define STOPLESSGC_ONLY(x) x\n'
    r'#define STOPLESSGC_ONLY_ARG(arg) arg,\n'
    r'#else\n'
    r'#define STOPLESSGC_ONLY(x)\n'
    r'#define STOPLESSGC_ONLY_ARG(arg)\n'
    r'#endif // INCLUDE_STOPLESSGC\n',
    s, count=1, flags=re.DOTALL)
if new == s: sys.exit("macros.hpp: STOPLESSGC block not added")
open(p, "w").write(new)
PY

# 7. JvmFeatures.gmk
python3 - <<'PY'
import re, sys
p = "make/hotspot/lib/JvmFeatures.gmk"
s = open(p).read()
# Find epsilongc block end "endif\n\n" before g1gc block
new = re.sub(
    r'(ifneq \(\$\(call check-jvm-feature, epsilongc\), true\)\n'
    r'.*?\bendif\n\n)',
    r'\1'
    r'ifneq ($(call check-jvm-feature, stoplessgc), true)\n'
    r'  JVM_CFLAGS_FEATURES += -DINCLUDE_STOPLESSGC=0\n'
    r'  JVM_EXCLUDE_PATTERNS += gc/stopless\n'
    r'else\n'
    r'  JVM_CFLAGS_FEATURES += -DINCLUDE_STOPLESSGC=1\n'
    r'  STOPLESS_RUNTIME_DIR ?= $(TOPDIR)/../cap_runtime/stopless_gc\n'
    r'  ifneq ($(wildcard $(STOPLESS_RUNTIME_DIR)/libstopless_gc.a),)\n'
    r'    JVM_LIBS_FEATURES += $(STOPLESS_RUNTIME_DIR)/libstopless_gc.a -lcheri_caprevoke\n'
    r'    JVM_CFLAGS_FEATURES += -I$(STOPLESS_RUNTIME_DIR)\n'
    r'  else\n'
    r'    $(warning STOPLESSGC enabled but $(STOPLESS_RUNTIME_DIR)/libstopless_gc.a not built)\n'
    r'    $(warning Run: make -C $(STOPLESS_RUNTIME_DIR) CROSS=1)\n'
    r'  endif\n'
    r'endif\n\n',
    s, count=1, flags=re.DOTALL)
if new == s: sys.exit("JvmFeatures.gmk: stoplessgc block not added")
open(p, "w").write(new)
PY

# 8. jvm-features.m4
python3 - <<'PY'
import re, sys
p = "make/autoconf/jvm-features.m4"
s = open(p).read()
new = re.sub(
    r'(cds compiler1 compiler2 dtrace epsilongc)(\s)',
    r'\1 stoplessgc\2', s, count=1)
if new == s: sys.exit("jvm-features.m4: stoplessgc not added")
open(p, "w").write(new)
PY

# Print the diff for capture
git diff src/hotspot/share/gc/shared/collectedHeap.hpp \
         src/hotspot/share/gc/shared/barrierSet.hpp \
         src/hotspot/share/gc/shared/gc_globals.hpp \
         src/hotspot/share/gc/shared/gcConfig.cpp \
         src/hotspot/share/runtime/arguments.cpp \
         src/hotspot/share/utilities/macros.hpp \
         make/hotspot/lib/JvmFeatures.gmk \
         make/autoconf/jvm-features.m4
