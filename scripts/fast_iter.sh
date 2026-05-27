#!/usr/bin/env bash
# scripts/fast_iter.sh — fast HotSpot edit→build→deploy→test cycle.
#
# Skips the JDK images step and only rebuilds + ships libjvm.so to the
# CheriBSD QEMU guest. Typical cycle ~3-5 min vs the old ~10-15 min.
#
# Usage:
#   scripts/fast_iter.sh                # full cycle
#   scripts/fast_iter.sh --build-only   # build libjvm.so on hasee; skip deploy/run
#   scripts/fast_iter.sh --no-build     # skip build; just redeploy what's there
#   scripts/fast_iter.sh --flags="..."  # pass extra java flags
#
# Assumes:
#   - hasee SSH access as bc@hasee
#   - QEMU CheriBSD listening on hasee:10005 (root@localhost from hasee's POV)
#   - /opt/jdk already extracted in QEMU guest from a previous full run

set -euo pipefail

BUILD=1
DEPLOY=1
EXTRA_FLAGS="${EXTRA_FLAGS:-}"

# parse args
for arg in "$@"; do
  case "$arg" in
    --build-only) DEPLOY=0 ;;
    --no-build)   BUILD=0 ;;
    --flags=*)    EXTRA_FLAGS="${arg#--flags=}" ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

# Default java args: -XX:+UnlockExperimentalVMOptions -XX:+UseEpsilonGC ... -version
DEFAULT_JAVA_ARGS="-XX:+UnlockExperimentalVMOptions -XX:+UseEpsilonGC -XX:-UseCompressedClassPointers -Xms32m -Xmx64m"
JAVA_CMD="/opt/jdk/bin/java ${DEFAULT_JAVA_ARGS} ${EXTRA_FLAGS} -version"

HASEE="bc@hasee"
JDK_BUILD="\$HOME/projs/stopless-java-gc/third_party/openjdk-jdk17/build/jdk-morello"
LIBJVM_PATH="${JDK_BUILD}/jdk/lib/server/libjvm.so"

# QEMU guest paths
GUEST_LIBJVM="/opt/jdk/lib/server/libjvm.so"
QEMU_SSH="ssh -p 10005 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@localhost"
QEMU_SCP="scp -P 10005 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

elapsed() { local t0=$1; printf '%5ds' $(( SECONDS - t0 )); }
log()     { printf "\033[1;36m[fast-iter %s]\033[0m %s\n" "$(elapsed $T0)" "$*"; }
warn()    { printf "\033[1;33m[fast-iter WARN]\033[0m %s\n" "$*"; }
die()     { printf "\033[1;31m[fast-iter FAIL]\033[0m %s\n" "$*" >&2; exit 1; }

T0=$SECONDS

# ---- BUILD (on hasee) -----------------------------------------------------
if (( BUILD )); then
  log "building hotspot-server-libs on hasee..."
  TBUILD=$SECONDS
  ssh "${HASEE}" "cd ${JDK_BUILD} && gmake CONF_CHECK=ignore hotspot-server-libs 2>&1 | tail -5" || die "build failed"
  log "build done ($(( SECONDS - TBUILD ))s)"

  log "verifying libjvm.so exists..."
  ssh "${HASEE}" "ls -la ${LIBJVM_PATH}" || die "libjvm.so not produced"
fi

# ---- DEPLOY + RUN ---------------------------------------------------------
if (( DEPLOY )); then
  log "shipping libjvm.so to QEMU guest (stripping first)..."
  TSHIP=$SECONDS
  # On hasee: strip libjvm.so into /tmp, then scp the stripped copy (~12 MB vs ~130 MB)
  STRIP_TOOL="\$HOME/projs/stopless-java-gc/third_party/output/morello-sdk/bin/aarch64-unknown-freebsd-strip"
  ssh "${HASEE}" "cp ${LIBJVM_PATH} /tmp/libjvm.so && ${STRIP_TOOL} /tmp/libjvm.so 2>&1 | tail -1 && ls -la /tmp/libjvm.so" 2>&1 | tail -2 || die "strip failed"
  ssh "${HASEE}" "${QEMU_SCP} /tmp/libjvm.so root@localhost:${GUEST_LIBJVM}" 2>&1 | tail -1 || die "scp failed"
  log "ship done ($(( SECONDS - TSHIP ))s)"

  log "running java in QEMU..."
  TRUN=$SECONDS
  ssh "${HASEE}" "${QEMU_SSH} 'rm -f /root/hs_err_pid* /opt/hs_err_pid*; ${JAVA_CMD} 2>&1; echo exit=\$?; ls /root/hs_err_pid*.log /opt/hs_err_pid*.log 2>/dev/null | head -1'" || true
  log "run done ($(( SECONDS - TRUN ))s)"

  log "checking for hs_err..."
  HSERR=$(ssh "${HASEE}" "${QEMU_SSH} 'ls -t /root/hs_err_pid*.log /opt/hs_err_pid*.log 2>/dev/null | head -1'" || true)
  if [[ -n "${HSERR}" ]]; then
    log "hs_err: ${HSERR}"
    ssh "${HASEE}" "${QEMU_SSH} 'head -40 ${HSERR}'" | grep -E "SIGSEGV|SIGBUS|SIGPROT|Problematic frame|elapsed time"
  fi
fi

log "total cycle: $(( SECONDS - T0 ))s"
