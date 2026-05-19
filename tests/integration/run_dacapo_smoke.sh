#!/usr/bin/env bash
# tests/integration/run_dacapo_smoke.sh — run DaCapo h2 for 1 iteration on
# the patched JDK under Morello FVP. Used as a pass/fail gate for Phase 1.
#
# This is *not* a benchmark; for benchmarking see scripts/run_benchmarks.sh.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JDK="${REPO_ROOT}/build/jdk-morello/images/jdk/bin/java"
DACAPO="${REPO_ROOT}/third_party/dacapo/dacapo-23.11-MR1-chopin.jar"

die() { printf '\033[1;31m[smoke error]\033[0m %s\n' "$*" >&2; exit 1; }

[[ -x "${JDK}"    ]] || die "patched JDK missing; run scripts/build_jdk.sh"
[[ -f "${DACAPO}" ]] || die "DaCapo jar missing; re-run scripts/bootstrap.sh"

"${REPO_ROOT}/scripts/run_in_fvp.sh" \
    "${JDK}" \
    -XX:+UnlockExperimentalVMOptions -XX:+UseZGC \
    -Xmx1g \
    -jar "${DACAPO}" h2 -n 1
