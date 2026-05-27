#!/usr/bin/env python3
"""
apply_stoplessgc_to_openjdk.py — make all C-2 edits to a checked-out
openjdk-jdk17u tree, using exact-string matching tuned to the real
file contents (no regex guesswork).

Pre-requisites:
  * 0080-stopless-gc-skeleton.patch already applied (the new files
    in src/hotspot/share/gc/stopless/ exist).
  * collectedHeap.hpp already has the Stopless enum entry (or this
    script will add it if missing).

Idempotent: re-running is safe — each edit is no-op if the marker
text already present.

Run from openjdk source root:
  cd third_party/openjdk-jdk17
  python3 ../../scripts/apply_stoplessgc_to_openjdk.py
"""
import sys
from pathlib import Path


def edit_file(rel_path, find, replace, marker=None, required=True):
    """Replace `find` with `replace` in `rel_path`. If `marker` already
    present in the file, skip (idempotent). Aborts on missing target."""
    p = Path(rel_path)
    if not p.exists():
        if required:
            sys.exit(f"missing file: {rel_path}")
        return False
    text = p.read_text()
    if marker is not None and marker in text:
        print(f"  skip  {rel_path}: marker present")
        return False
    if find not in text:
        sys.exit(f"  FAIL  {rel_path}: anchor not found:\n    {find[:80]!r}...")
    new = text.replace(find, replace, 1)
    p.write_text(new)
    print(f"  done  {rel_path}")
    return True


# ============================================================
# 1. collectedHeap.hpp — enum Name += Stopless
# ============================================================
print("[1/8] collectedHeap.hpp")
edit_file(
    "src/hotspot/share/gc/shared/collectedHeap.hpp",
    find=    "    Shenandoah\n  };",
    replace= "    Shenandoah,\n    Stopless\n  };",
    marker=  "Stopless\n  };",
)

# ============================================================
# 2. barrierSetConfig.hpp — FOR_EACH_CONCRETE_BARRIER_SET_DO
# ============================================================
print("[2/8] barrierSetConfig.hpp")
edit_file(
    "src/hotspot/share/gc/shared/barrierSetConfig.hpp",
    find=    "  ZGC_ONLY(f(ZBarrierSet))",
    replace= "  ZGC_ONLY(f(ZBarrierSet))                           \\\n"
             "  STOPLESSGC_ONLY(f(StoplessBarrierSet))",
    marker=  "STOPLESSGC_ONLY(f(StoplessBarrierSet))",
)

# ============================================================
# 3. gc_globals.hpp — include + UseStoplessGC product flag
# ============================================================
print("[3/8] gc_globals.hpp")
# (a) include
edit_file(
    "src/hotspot/share/gc/shared/gc_globals.hpp",
    find=    '#if INCLUDE_EPSILONGC\n'
             '#include "gc/epsilon/epsilon_globals.hpp"\n'
             '#endif',
    replace= '#if INCLUDE_EPSILONGC\n'
             '#include "gc/epsilon/epsilon_globals.hpp"\n'
             '#endif\n'
             '#if INCLUDE_STOPLESSGC\n'
             '#include "gc/stopless/stopless_globals.hpp"\n'
             '#endif',
    marker=  '#include "gc/stopless/stopless_globals.hpp"',
)
# (b) UseStoplessGC product flag
edit_file(
    "src/hotspot/share/gc/shared/gc_globals.hpp",
    find=    '  product(bool, UseShenandoahGC, false,                                     \\\n'
             '          "Use the Shenandoah garbage collector")                           \\\n'
             '                                                                            \\\n',
    replace= '  product(bool, UseShenandoahGC, false,                                     \\\n'
             '          "Use the Shenandoah garbage collector")                           \\\n'
             '                                                                            \\\n'
             '  product(bool, UseStoplessGC, false, EXPERIMENTAL,                         \\\n'
             '          "Use the Stopless (CHERI-accelerated) garbage collector")         \\\n'
             '                                                                            \\\n',
    marker=  'UseStoplessGC',
)

# ============================================================
# 4. gcConfig.cpp — include + static + IncludedGC table + FAIL_IF
# ============================================================
print("[4/8] gcConfig.cpp")
# (a) include
edit_file(
    "src/hotspot/share/gc/shared/gcConfig.cpp",
    find=    '#if INCLUDE_ZGC\n'
             '#include "gc/z/zArguments.hpp"\n'
             '#endif',
    replace= '#if INCLUDE_ZGC\n'
             '#include "gc/z/zArguments.hpp"\n'
             '#endif\n'
             '#if INCLUDE_STOPLESSGC\n'
             '#include "gc/stopless/stoplessArguments.hpp"\n'
             '#endif',
    marker=  '#include "gc/stopless/stoplessArguments.hpp"',
)
# (b) static StoplessArguments
edit_file(
    "src/hotspot/share/gc/shared/gcConfig.cpp",
    find=    "         ZGC_ONLY(static ZArguments          zArguments;)\n",
    replace= "         ZGC_ONLY(static ZArguments          zArguments;)\n"
             "   STOPLESSGC_ONLY(static StoplessArguments  stoplessArguments;)\n",
    marker=  "STOPLESSGC_ONLY(static StoplessArguments",
)
# (c) IncludedGCs table entry
edit_file(
    "src/hotspot/share/gc/shared/gcConfig.cpp",
    find=    '         ZGC_ONLY_ARG(IncludedGC(UseZGC,             CollectedHeap::Z,          zArguments,          "z gc"))\n'
             '};',
    replace= '         ZGC_ONLY_ARG(IncludedGC(UseZGC,             CollectedHeap::Z,          zArguments,          "z gc"))\n'
             '   STOPLESSGC_ONLY_ARG(IncludedGC(UseStoplessGC,     CollectedHeap::Stopless,   stoplessArguments,   "stopless gc"))\n'
             '};',
    marker=  "STOPLESSGC_ONLY_ARG(IncludedGC(UseStoplessGC",
)
# (d) fail_if_non_included_gc_is_selected: add NOT_STOPLESSGC entry
edit_file(
    "src/hotspot/share/gc/shared/gcConfig.cpp",
    find=    "  NOT_ZGC(         FAIL_IF_SELECTED(UseZGC));",
    replace= "  NOT_ZGC(         FAIL_IF_SELECTED(UseZGC));\n"
             "  NOT_STOPLESSGC(  FAIL_IF_SELECTED(UseStoplessGC));",
    marker=  "NOT_STOPLESSGC(  FAIL_IF_SELECTED",
)

# ============================================================
# 5. macros.hpp — STOPLESSGC_ONLY block
# ============================================================
print("[5/8] macros.hpp")
edit_file(
    "src/hotspot/share/utilities/macros.hpp",
    find=    "#define NOT_EPSILONGC_RETURN_(code) { return code; }\n"
             "#endif // INCLUDE_EPSILONGC\n",
    replace= "#define NOT_EPSILONGC_RETURN_(code) { return code; }\n"
             "#endif // INCLUDE_EPSILONGC\n"
             "\n"
             "#ifndef INCLUDE_STOPLESSGC\n"
             "#define INCLUDE_STOPLESSGC 1\n"
             "#endif // INCLUDE_STOPLESSGC\n"
             "\n"
             "#if INCLUDE_STOPLESSGC\n"
             "#define STOPLESSGC_ONLY(x) x\n"
             "#define STOPLESSGC_ONLY_ARG(arg) arg,\n"
             "#define NOT_STOPLESSGC(x)\n"
             "#define NOT_STOPLESSGC_RETURN        /* next token must be ; */\n"
             "#define NOT_STOPLESSGC_RETURN_(code) /* next token must be ; */\n"
             "#else\n"
             "#define STOPLESSGC_ONLY(x)\n"
             "#define STOPLESSGC_ONLY_ARG(arg)\n"
             "#define NOT_STOPLESSGC(x) x\n"
             "#define NOT_STOPLESSGC_RETURN        {}\n"
             "#define NOT_STOPLESSGC_RETURN_(code) { return code; }\n"
             "#endif // INCLUDE_STOPLESSGC\n",
    marker=  "#ifndef INCLUDE_STOPLESSGC",
)

# ============================================================
# 6. JvmFeatures.gmk — stoplessgc feature block
# ============================================================
print("[6/8] JvmFeatures.gmk")
edit_file(
    "make/hotspot/lib/JvmFeatures.gmk",
    find=    "ifneq ($(call check-jvm-feature, epsilongc), true)\n"
             "  JVM_CFLAGS_FEATURES += -DINCLUDE_EPSILONGC=0\n"
             "  JVM_EXCLUDE_PATTERNS += gc/epsilon\n"
             "endif\n",
    replace= "ifneq ($(call check-jvm-feature, epsilongc), true)\n"
             "  JVM_CFLAGS_FEATURES += -DINCLUDE_EPSILONGC=0\n"
             "  JVM_EXCLUDE_PATTERNS += gc/epsilon\n"
             "endif\n"
             "\n"
             "ifneq ($(call check-jvm-feature, stoplessgc), true)\n"
             "  JVM_CFLAGS_FEATURES += -DINCLUDE_STOPLESSGC=0\n"
             "  JVM_EXCLUDE_PATTERNS += gc/stopless\n"
             "else\n"
             "  JVM_CFLAGS_FEATURES += -DINCLUDE_STOPLESSGC=1\n"
             "  STOPLESS_RUNTIME_DIR ?= $(TOPDIR)/../cap_runtime/stopless_gc\n"
             "  ifneq ($(wildcard $(STOPLESS_RUNTIME_DIR)/libstopless_gc.a),)\n"
             "    JVM_LIBS_FEATURES += $(STOPLESS_RUNTIME_DIR)/libstopless_gc.a -lcheri_caprevoke\n"
             "    JVM_CFLAGS_FEATURES += -I$(STOPLESS_RUNTIME_DIR)\n"
             "  else\n"
             "    $(warning STOPLESSGC enabled but $(STOPLESS_RUNTIME_DIR)/libstopless_gc.a not built)\n"
             "    $(warning Run: gmake -C $(STOPLESS_RUNTIME_DIR) CROSS=1)\n"
             "  endif\n"
             "endif\n",
    marker=  "check-jvm-feature, stoplessgc",
)

# ============================================================
# 7. jvm-features.m4 — add stoplessgc to valid features
# ============================================================
print("[7/8] jvm-features.m4")
edit_file(
    "make/autoconf/jvm-features.m4",
    find=    "    cds compiler1 compiler2 dtrace epsilongc g1gc jfr jni-check",
    replace= "    cds compiler1 compiler2 dtrace epsilongc stoplessgc g1gc jfr jni-check",
    marker=  "epsilongc stoplessgc",
)
# Also add a description macro near epsilongc's
edit_file(
    "make/autoconf/jvm-features.m4",
    find=    "m4_define(jvm_feature_desc_epsilongc, [include the epsilon (no-op) garbage collector])\n",
    replace= "m4_define(jvm_feature_desc_epsilongc, [include the epsilon (no-op) garbage collector])\n"
             "m4_define(jvm_feature_desc_stoplessgc, [include the Stopless (CHERI-accelerated) garbage collector])\n",
    marker=  "jvm_feature_desc_stoplessgc",
)

# ============================================================
# 8. final summary
# ============================================================
print("[8/8] all edits attempted")
print("Now run: git diff > /tmp/0081-real.patch  (from openjdk source root)")
