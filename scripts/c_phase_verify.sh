#!/usr/bin/env bash
# scripts/c_phase_verify.sh — verify Phase C-1..C-4 on hasee + QEMU.
#
# Stages (each is independently runnable via --only <stage>):
#   A. cap_runtime: build + ship + run 4 C-level tests on QEMU
#        (test_basic, test_multi — regression)
#        (test_alloc, test_alloc_concurrent — Phase C-4 new)
#   B. apply_patches.sh: dry-run check each of 0080..0085 patches
#   C. hotspot build: fast_iter.sh --build-only with -XX:+UseStoplessGC
#   D. JVM run: java -XX:+UseStoplessGC -version on QEMU
#
# Usage:
#   scripts/c_phase_verify.sh                  # run all stages A→D
#   scripts/c_phase_verify.sh --only A         # only the C-runtime tests
#   scripts/c_phase_verify.sh --only A,B       # comma-list of stages
#   scripts/c_phase_verify.sh --keep-going     # don't abort on first failure
#
# Assumes the same environment as scripts/fast_iter.sh:
#   - SSH to bc@hasee
#   - SSH from hasee to root@localhost:10005 (QEMU CheriBSD guest)
#   - cap_runtime/stopless_gc has been Makefile-rebuilt at least once
#     on hasee.

set -uo pipefail

# --------------------------------------------------------------------------
# config
# --------------------------------------------------------------------------
HASEE="bc@hasee"
QEMU_SSH='ssh -p 10005 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@localhost'
QEMU_SCP='scp -P 10005 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null'

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HASEE_REPO='$HOME/projs/stopless-java-gc'
CAP_RT_DIR="${HASEE_REPO}/src/cap_runtime/stopless_gc"

STAGES_ALL=(A B C D)
STAGES=()
KEEP_GOING=0

# --------------------------------------------------------------------------
# argument parsing
# --------------------------------------------------------------------------
for arg in "$@"; do
  case "$arg" in
    --only=*)
      IFS=',' read -ra REQ <<< "${arg#--only=}"
      STAGES=("${REQ[@]}")
      ;;
    --only)
      shift; IFS=',' read -ra REQ <<< "$1"
      STAGES=("${REQ[@]}")
      ;;
    --keep-going) KEEP_GOING=1 ;;
    -h|--help)
      grep '^# ' "$0" | head -25 | sed 's/^# //'
      exit 0
      ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done
if [[ ${#STAGES[@]} -eq 0 ]]; then STAGES=("${STAGES_ALL[@]}"); fi

# --------------------------------------------------------------------------
# logging helpers
# --------------------------------------------------------------------------
T0=$SECONDS
elapsed() { printf '%4ds' $(( SECONDS - T0 )); }
log()     { printf '\033[1;36m[c-verify %s]\033[0m %s\n' "$(elapsed)" "$*"; }
warn()    { printf '\033[1;33m[c-verify %s WARN]\033[0m %s\n' "$(elapsed)" "$*"; }
pass()    { printf '\033[1;32m[c-verify %s  OK ]\033[0m %s\n' "$(elapsed)" "$*"; }
fail()    { printf '\033[1;31m[c-verify %s FAIL]\033[0m %s\n' "$(elapsed)" "$*"; }

# Per-stage state
declare -A STAGE_STATUS
declare -A STAGE_TIME

run_stage() {
  local id="$1" title="$2" fn="$3"
  local t0=$SECONDS
  log "=== Stage ${id}: ${title} ==="
  if "$fn"; then
    STAGE_STATUS[$id]="PASS"
    STAGE_TIME[$id]=$(( SECONDS - t0 ))
    pass "Stage ${id} (${STAGE_TIME[$id]}s)"
    return 0
  else
    STAGE_STATUS[$id]="FAIL"
    STAGE_TIME[$id]=$(( SECONDS - t0 ))
    fail "Stage ${id} (${STAGE_TIME[$id]}s)"
    [[ ${KEEP_GOING} -eq 1 ]] && return 0 || return 1
  fi
}

# --------------------------------------------------------------------------
# Stage A: C-runtime tests
# --------------------------------------------------------------------------
stage_A() {
  log "A.0 — rebuild libstopless_gc.a + test binaries on hasee"
  ssh "${HASEE}" "cd ${CAP_RT_DIR} && gmake CROSS=1 clean && gmake CROSS=1 2>&1 | tail -10" \
    || { warn "cap_runtime build failed"; return 1; }

  log "A.1 — ship binaries to QEMU guest"
  local bins=(test_basic test_multi test_alloc test_alloc_concurrent)
  for b in "${bins[@]}"; do
    ssh "${HASEE}" "test -x ${CAP_RT_DIR}/${b}" \
      || { warn "missing binary: ${b}"; return 1; }
    ssh "${HASEE}" "${QEMU_SCP} ${CAP_RT_DIR}/${b} root@localhost:/root/${b}" \
      > /dev/null 2>&1 \
      || { warn "scp ${b} failed"; return 1; }
  done

  local rc=0
  for b in "${bins[@]}"; do
    log "A.2 — running ${b} on QEMU"
    local out
    out=$(ssh "${HASEE}" "${QEMU_SSH} 'cd /root && ./${b} 2>&1; echo __exit=\$?'" 2>&1) || true
    echo "${out}" | sed 's/^/    /'
    local exit_line
    exit_line=$(echo "${out}" | grep -E '^__exit=' | tail -1)
    if [[ "${exit_line}" == "__exit=0" ]]; then
      pass "${b}: exit 0"
    else
      fail "${b}: ${exit_line:-no exit line}"
      rc=1
    fi
  done
  return $rc
}

# --------------------------------------------------------------------------
# Stage B: patch apply dry-run
# --------------------------------------------------------------------------
stage_B() {
  log "B.0 — git apply --check each Phase-C patch"
  local patches=(
    0080-stopless-gc-skeleton.patch
    0081-stopless-gc-feature-enable.patch
    0082-stopless-runtime-link.patch
    0083-stopless-arena-cpp-bridge.patch
    0085-stopless-arena-allocate-wire.patch
  )

  local rc=0
  for p in "${patches[@]}"; do
    local result
    result=$(ssh "${HASEE}" "cd ${HASEE_REPO}/third_party/openjdk-jdk17 && \
      git apply --check ${HASEE_REPO}/patches/openjdk-jdk17/${p} 2>&1" || echo "__REJECT__")
    if [[ "${result}" == "__REJECT__" || "${result}" == *"error"* ]]; then
      fail "${p}: REJECT"
      echo "${result}" | head -10 | sed 's/^/    /'
      rc=1
    else
      pass "${p}: applies clean"
    fi
  done

  if [[ $rc -ne 0 ]]; then
    warn "Some patches need rebase. Apply manually on hasee, then regen via 'git diff'."
  fi
  return $rc
}

# --------------------------------------------------------------------------
# Stage C: hotspot build with stoplessgc
# --------------------------------------------------------------------------
stage_C() {
  log "C.0 — ensure all patches applied (idempotent)"
  ssh "${HASEE}" "cd ${HASEE_REPO} && bash scripts/apply_patches.sh 2>&1 | tail -5" \
    || { warn "apply_patches failed"; return 1; }

  log "C.1 — build hotspot-server-libs"
  local build_log
  build_log=$(ssh "${HASEE}" "cd ${HASEE_REPO}/third_party/openjdk-jdk17/build/jdk-morello && \
    gmake CONF_CHECK=ignore hotspot-server-libs 2>&1 | tail -20")
  echo "${build_log}" | sed 's/^/    /'

  if echo "${build_log}" | grep -qE 'Error|error:' ; then
    fail "build errors detected"
    return 1
  fi

  log "C.2 — verify libjvm.so was produced"
  ssh "${HASEE}" "ls -la ${HASEE_REPO}/third_party/openjdk-jdk17/build/jdk-morello/jdk/lib/server/libjvm.so" \
    || { warn "libjvm.so missing"; return 1; }
  return 0
}

# --------------------------------------------------------------------------
# Stage D: JVM smoke test
# --------------------------------------------------------------------------
stage_D() {
  log "D.0 — fast_iter ship + run with -XX:+UseStoplessGC -version"
  local out
  out=$(EXTRA_FLAGS='-XX:+UnlockExperimentalVMOptions -XX:+UseStoplessGC' \
        bash "${REPO_ROOT}/scripts/fast_iter.sh" --no-build 2>&1) || true
  echo "${out}" | tail -20 | sed 's/^/    /'

  if echo "${out}" | grep -qE 'openjdk version|java version'; then
    pass "JVM banner printed"
    return 0
  elif echo "${out}" | grep -qE 'SIGPROT|SIGSEGV|SIGBUS'; then
    fail "JVM crashed (probably shift=64 — see C-6 task #30)"
    return 1
  else
    warn "no version banner and no crash — inspect output above"
    return 1
  fi
}

# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
log "stages requested: ${STAGES[*]}"
overall_rc=0
for s in "${STAGES[@]}"; do
  case "$s" in
    A) run_stage A "cap_runtime C tests"           stage_A || overall_rc=1 ;;
    B) run_stage B "openjdk patch dry-run"         stage_B || overall_rc=1 ;;
    C) run_stage C "hotspot build w/ StoplessGC"   stage_C || overall_rc=1 ;;
    D) run_stage D "java -XX:+UseStoplessGC -ver"  stage_D || overall_rc=1 ;;
    *) warn "unknown stage: $s" ;;
  esac
done

echo
echo "=========================================="
echo " c_phase_verify summary"
echo "=========================================="
for s in "${STAGES[@]}"; do
  printf "  Stage %s: %s (%ss)\n" "$s" "${STAGE_STATUS[$s]:-SKIP}" "${STAGE_TIME[$s]:-0}"
done
echo "  Total time: $(( SECONDS - T0 ))s"
echo "=========================================="

exit ${overall_rc}
