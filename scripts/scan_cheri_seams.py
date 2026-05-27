#!/usr/bin/env python3
"""scan_cheri_seams.py — enumerate likely CHERI seams in HotSpot source.

Looks for the recurring "8-byte machine word" assumption patterns:

  Category A:  X * BytesPerWord            (word-count -> byte-count, often broken)
  Category B:  X << LogBytesPerWord        (same as A, shift form)
  Category C:  X * sizeof(jlong)           (assumes pointer == jlong)
  Category D:  N / BytesPerWord            (byte-count -> word-count)
  Category E:  intptr_signature            (Java-side cap holder, treats cap as long)
  Category F:  hardcoded * 8 in size/offset context
  Category G:  memset(dst, _, N*BytesPerWord)  (size mismatch potential)
  Category H:  oop / Klass / Metadata cast from integer (cap provenance loss)

Output: a markdown report at docs/10_cheri_seam_scan.md grouped by category
and HotSpot subsystem. Each entry has file:line and the source line itself.
"""
import os, re, sys, subprocess
from collections import defaultdict
from pathlib import Path

# ----- config --------------------------------------------------------------
HOTSPOT_ROOT = Path(os.path.expanduser(
    "~/projects/stopless-java-gc/third_party/openjdk-jdk17/src/hotspot"
))
# fallback to hasee remote tree via local mirror if needed (we won't here)
OUTPUT = Path(os.path.expanduser(
    "~/projects/stopless-java-gc/docs/10_cheri_seam_scan.md"
))

# Subsystems to organise results by
SUBSYSTEMS = [
    ("classfile",  "src/hotspot/share/classfile"),
    ("oops",       "src/hotspot/share/oops"),
    ("memory",     "src/hotspot/share/memory"),
    ("gc-shared",  "src/hotspot/share/gc/shared"),
    ("gc-z",       "src/hotspot/share/gc/z"),
    ("gc-other",   "src/hotspot/share/gc"),
    ("runtime",   "src/hotspot/share/runtime"),
    ("interp",    "src/hotspot/share/interpreter"),
    ("c1",        "src/hotspot/share/c1"),
    ("opto",      "src/hotspot/share/opto"),
    ("aarch64",   "src/hotspot/cpu/aarch64"),
    ("os-bsd",    "src/hotspot/os/bsd"),
    ("oscpu-bsd-aarch64", "src/hotspot/os_cpu/bsd_aarch64"),
    ("utils",     "src/hotspot/share/utilities"),
]

# ----- patterns ------------------------------------------------------------
PATTERNS = [
    # Category A — word_size * BytesPerWord  (CHERI-broken when word_size is in MetaWord/HeapWord units)
    ("A", r"\b\w+\s*\*\s*BytesPerWord\b",                    "word_count * BytesPerWord (likely buggy on CHERI)"),
    ("A", r"\bBytesPerWord\s*\*\s*\w+\b",                    "BytesPerWord * word_count (likely buggy on CHERI)"),
    # Category B — X << LogBytesPerWord
    ("B", r"<<\s*LogBytesPerWord\b",                          "shift by LogBytesPerWord (== *8 under LP64)"),
    ("B", r"\bLogBytesPerWord\b",                             "LogBytesPerWord referenced"),
    # Category C — sizeof(jlong) used as pointer size
    ("C", r"\bsizeof\s*\(\s*jlong\s*\)",                      "sizeof(jlong): 8 bytes; if used as ptr size, wrong on CHERI"),
    # Category D — / BytesPerWord (byte->word conversion)
    ("D", r"/\s*BytesPerWord\b",                              "divide by BytesPerWord (inverse conversion)"),
    # Category E — intptr_signature (Java long-as-pointer)
    ("E", r"intptr_signature",                                "intptr_signature (cap as Java long; cf. T_ADDRESS fix)"),
    # Category F — Hardcoded *8 / <<3 in size contexts (heuristic)
    ("F", r"\*\s*8\s*[,)]",                                   "literal *8 (possible word-size assumption)"),
    ("F", r"<<\s*3\s*\b",                                     "literal <<3 (== *8)"),
    # Category G — memset/memcpy with BytesPerWord in size
    ("G", r"\bmemset\s*\([^;]*BytesPerWord",                  "memset with BytesPerWord in size arg"),
    ("G", r"\bmemcpy\s*\([^;]*BytesPerWord",                  "memcpy with BytesPerWord in size arg"),
    # Category H — casts from integer to pointer types (provenance loss)
    ("H", r"\(\s*Klass\s*\*\s*\)\s*\w+(?!->)",                "cast to Klass* (cap provenance check)"),
    ("H", r"\(\s*Metadata\s*\*\s*\)",                          "cast to Metadata* (cap provenance check)"),
    ("H", r"\(\s*Method\s*\*\s*\)\s*\w+(?!->)",                "cast to Method* (cap provenance check)"),
]

# Lines to suppress (false positives / comments / unrelated)
SUPPRESS_LINE = re.compile(r"^\s*(//|\*|#)|/\*|FALSE|hotspot-symbols|test")

# ----- scan ---------------------------------------------------------------
def scan():
    results = defaultdict(list)  # (subsys, category) -> [(path, lineno, snippet, hint)]
    if not HOTSPOT_ROOT.exists():
        print(f"FATAL: HotSpot root {HOTSPOT_ROOT} not found.")
        print("Tip: this script can also be run against a remote tree via the wrapper:")
        print("  ssh bc@hasee 'python3 - < this_script' > docs/10_cheri_seam_scan.md")
        sys.exit(2)

    pat_compiled = [(cat, re.compile(rx), hint) for cat, rx, hint in PATTERNS]

    for subsys, rel in SUBSYSTEMS:
        full = HOTSPOT_ROOT.parent.parent / rel
        if not full.exists():
            continue
        for path in full.rglob("*"):
            if path.is_dir(): continue
            if path.suffix not in (".cpp", ".hpp", ".inline.hpp", ".h"):  continue
            try:
                with open(path, encoding="utf-8", errors="replace") as f:
                    for n, line in enumerate(f, 1):
                        if SUPPRESS_LINE.match(line): continue
                        # Skip lines that look like comments after code (best-effort)
                        stripped = line.split("//", 1)[0].split("/*", 1)[0]
                        if not stripped.strip(): continue
                        for cat, pat, hint in pat_compiled:
                            if pat.search(stripped):
                                rel_path = str(path.relative_to(HOTSPOT_ROOT.parent.parent))
                                results[(subsys, cat)].append((rel_path, n, line.rstrip(), hint))
                                break  # one cat per line is enough
            except Exception as e:
                print(f"warn: {path}: {e}", file=sys.stderr)
    return results

# ----- render -------------------------------------------------------------
CAT_NAMES = {
    "A": "Word-count * BytesPerWord (most common bug pattern)",
    "B": "Shift by LogBytesPerWord (== *8 on LP64)",
    "C": "sizeof(jlong) used as pointer size",
    "D": "Divide by BytesPerWord (byte->word)",
    "E": "intptr_signature (Java long-as-pointer)",
    "F": "Literal *8 / <<3 (heuristic)",
    "G": "memset / memcpy with BytesPerWord in size arg",
    "H": "Casts to Klass* / Method* / Metadata*",
}

def render(results):
    lines = []
    lines.append("# CHERI seam scan — HotSpot source\n")
    lines.append(f"Generated by `scripts/scan_cheri_seams.py`.\n")
    lines.append("Each entry is a *candidate* CHERI seam: a site where HotSpot")
    lines.append("conflates a 'machine word' with 8 bytes, or where an integer-")
    lines.append("derived pointer might lose its CHERI capability tag. Many entries")
    lines.append("are harmless on standard LP64 but break (or already broke) under")
    lines.append("purecap. Use this as a *worklist* — verify each before patching.\n")

    # Counts per category, per subsystem
    counts = defaultdict(lambda: defaultdict(int))
    for (sub, cat), items in results.items():
        counts[cat][sub] = len(items)

    lines.append("## Summary by category × subsystem\n")
    cats_sorted = sorted(counts.keys())
    subs_sorted = sorted({s for cat in counts for s in counts[cat]})
    lines.append("| Category | " + " | ".join(subs_sorted) + " | **Total** |")
    lines.append("|" + "|".join(["---"] * (2 + len(subs_sorted))) + "|")
    for cat in cats_sorted:
        row = [f"**{cat}** {CAT_NAMES.get(cat,'?')[:32]}"]
        total = 0
        for sub in subs_sorted:
            n = counts[cat].get(sub, 0)
            total += n
            row.append(str(n) if n else "")
        row.append(f"**{total}**")
        lines.append("| " + " | ".join(row) + " |")
    lines.append("")

    # Detailed listings per category
    for cat in cats_sorted:
        lines.append(f"\n## Category {cat}: {CAT_NAMES.get(cat,'?')}\n")
        # Group by subsystem
        by_sub = defaultdict(list)
        for (sub, c), items in results.items():
            if c == cat:
                by_sub[sub].extend(items)
        for sub in sorted(by_sub):
            items = by_sub[sub]
            lines.append(f"### {sub} ({len(items)} hits)")
            lines.append("```")
            for path, n, line, hint in sorted(items)[:30]:
                # truncate long lines
                snippet = line.strip()
                if len(snippet) > 100: snippet = snippet[:97] + "..."
                lines.append(f"{path}:{n}  {snippet}")
            if len(items) > 30:
                lines.append(f"... and {len(items)-30} more")
            lines.append("```\n")

    return "\n".join(lines)

# ----- main ---------------------------------------------------------------
if __name__ == "__main__":
    results = scan()
    md = render(results)
    OUTPUT.write_text(md)
    total = sum(len(v) for v in results.values())
    print(f"Wrote {OUTPUT} ({total} hits, {len(results)} (subsys,cat) buckets).")
