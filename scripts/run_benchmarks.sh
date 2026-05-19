#!/usr/bin/env bash
# scripts/run_benchmarks.sh — run DaCapo / Renaissance / microbench on the
# patched JDK in Morello FVP, dump perf-counter timeseries + JFR.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JDK="${REPO_ROOT}/build/jdk-morello/images/jdk/bin/java"
DACAPO_JAR="${REPO_ROOT}/third_party/dacapo/dacapo-23.11-MR1-chopin.jar"
RESULTS_DIR="${REPO_ROOT}/bench/results/$(date -u +%Y%m%dT%H%M%SZ)"

log() { printf '\033[1;36m[bench]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[bench error]\033[0m %s\n' "$*" >&2; exit 1; }

[[ -x "${JDK}" ]] || die "Patched JDK missing at ${JDK}. Run scripts/build_jdk.sh."
[[ -f "${DACAPO_JAR}" ]] || die "DaCapo jar missing at ${DACAPO_JAR}. Re-run scripts/bootstrap.sh."

mkdir -p "${RESULTS_DIR}"

run_one() {
    local bench="$1" iters="${2:-5}"
    log "running DaCapo:${bench} × ${iters} iterations"
    local jfr="${RESULTS_DIR}/${bench}.jfr"
    local log_file="${RESULTS_DIR}/${bench}.log"
    "${REPO_ROOT}/scripts/run_in_fvp.sh" \
        "${JDK}" \
        -XX:+UnlockExperimentalVMOptions -XX:+UseZGC \
        -XX:+FlightRecorder \
        "-XX:StartFlightRecording=duration=600s,filename=${jfr},settings=profile" \
        -Xmx4g \
        -jar "${DACAPO_JAR}" "${bench}" -n "${iters}" \
        > "${log_file}" 2>&1 \
        || { log "DaCapo:${bench} failed (see ${log_file})"; return 1; }
    log "  → ${jfr}, ${log_file}"
}

# Default set: pointer-chasing benchmarks where load barrier cost matters
BENCHMARKS=("${@:-h2 pmd}")
for bench in ${BENCHMARKS[*]}; do
    run_one "${bench}"
done

log "all benchmarks done. Results in ${RESULTS_DIR}"
log "next: scripts/plot_results.py ${RESULTS_DIR}"
