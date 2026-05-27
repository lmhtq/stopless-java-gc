#!/usr/bin/env python3
"""
fix_patch_counts.py — recompute @@ hunk-line counts in unified diffs.

When you hand-write a patch, the `@@ -X,Y +U,V @@` header's Y (lines
in old file covered by this hunk) and V (lines in new) often drift
from the actual `+`/`-`/` ` line counts, and git apply rejects the
patch with "patch corrupt at line N".

This script reads each .patch argument, for each hunk:
  - counts ' ' and '-' lines → Y_new
  - counts ' ' and '+' lines → V_new
  - rewrites the @@ header in place

Idempotent. Use with care on patches you authored by hand.

Usage:
  python3 scripts/fix_patch_counts.py patches/openjdk-jdk17/0080*.patch
  python3 scripts/fix_patch_counts.py --check patches/openjdk-jdk17/0080*.patch
"""
import argparse
import re
import sys
from pathlib import Path

HUNK_RE = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)$")
DIFF_RE = re.compile(r"^diff --git ")


def fix_patch(path: Path, check_only: bool) -> bool:
    """Return True if patch was OK (or fixed); False if structural problem."""
    lines = path.read_text().splitlines(keepends=False)
    out = []
    i = 0
    n = len(lines)
    changed = False

    while i < n:
        line = lines[i]
        m = HUNK_RE.match(line)
        if not m:
            out.append(line)
            i += 1
            continue

        old_start = int(m.group(1))
        old_count_stated = int(m.group(2)) if m.group(2) is not None else 1
        new_start = int(m.group(3))
        new_count_stated = int(m.group(4)) if m.group(4) is not None else 1
        suffix = m.group(5)

        # Walk forward, counting until next diff or hunk.
        j = i + 1
        old_count = 0
        new_count = 0
        while j < n:
            ln = lines[j]
            if HUNK_RE.match(ln) or DIFF_RE.match(ln):
                break
            if ln.startswith("+") and not ln.startswith("+++"):
                new_count += 1
            elif ln.startswith("-") and not ln.startswith("---"):
                old_count += 1
            elif ln.startswith(" "):
                old_count += 1
                new_count += 1
            elif ln.startswith("\\"):
                # "\ No newline at end of file" — neutral
                pass
            else:
                # blank line in patch body — git treats as context " "
                if ln == "":
                    old_count += 1
                    new_count += 1
                else:
                    # unknown prefix; stop counting (safety)
                    break
            j += 1

        if (old_count, new_count) != (old_count_stated, new_count_stated):
            changed = True
            if check_only:
                print(f"{path.name}: hunk @@ -{old_start},{old_count_stated} "
                      f"+{new_start},{new_count_stated} -> "
                      f"should be -{old_start},{old_count} +{new_start},{new_count}",
                      file=sys.stderr)
            fixed = f"@@ -{old_start},{old_count} +{new_start},{new_count} @@{suffix}"
            out.append(fixed)
        else:
            out.append(line)

        # Copy hunk body verbatim.
        out.extend(lines[i + 1:j])
        i = j

    if changed and not check_only:
        path.write_text("\n".join(out) + "\n")
        print(f"fixed: {path}")
    elif changed and check_only:
        print(f"would-fix: {path}")
    else:
        print(f"ok:      {path}")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="print diffs but don't write")
    ap.add_argument("patches", nargs="+", type=Path)
    args = ap.parse_args()

    for p in args.patches:
        if not p.exists():
            print(f"missing: {p}", file=sys.stderr)
            sys.exit(2)
        fix_patch(p, args.check)


if __name__ == "__main__":
    main()
