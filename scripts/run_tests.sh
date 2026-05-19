#!/usr/bin/env bash
# scripts/run_tests.sh — drive the test suite.
#   --unit   : gtest against src/cap_runtime/ (host build, no JVM needed)
#   --integ  : jtreg + DaCapo smoke on patched JDK (needs FVP for Morello)
#   (none)   : run both

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

RUN_UNIT=1
RUN_INTEG=1

log() { printf '\033[1;36m[tests]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[tests error]\033[0m %s\n' "$*" >&2; exit 1; }

case "${1:-}" in
    --unit)   RUN_INTEG=0; shift || true ;;
    --integ)  RUN_UNIT=0;  shift || true ;;
    "")       ;;
    *)        die "unknown arg: $1" ;;
esac

run_unit() {
    log "unit tests (gtest, host build)"
    local build="${REPO_ROOT}/build/cap_runtime_tests"
    mkdir -p "${build}"
    cmake -S "${REPO_ROOT}" -B "${build}" -DCAP_RUNTIME_BUILD_TESTS=ON >/dev/null
    cmake --build "${build}" --target cap_runtime_tests
    ctest --test-dir "${build}" --output-on-failure
}

run_integ() {
    log "integration tests (smoke on FVP)"
    local jdk="${REPO_ROOT}/build/jdk-morello/images/jdk/bin/java"
    if [[ ! -x "${jdk}" ]]; then
        log "patched JDK not built; skipping. Run scripts/build_jdk.sh first."
        return
    fi
    "${REPO_ROOT}/scripts/run_in_fvp.sh" "${jdk}" -version
    # TODO(phase1): jtreg + DaCapo h2 smoke
}

[[ "${RUN_UNIT}"  -eq 1 ]] && run_unit
[[ "${RUN_INTEG}" -eq 1 ]] && run_integ
log "tests done"
