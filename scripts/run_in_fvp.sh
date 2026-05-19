#!/usr/bin/env bash
# scripts/run_in_fvp.sh — run a purecap-aarch64 binary in Morello FVP.
# Cycle-approximate; usable for paper-grade relative performance numbers.
#
# Usage: scripts/run_in_fvp.sh <binary> [args...]
#
# Strategy: boot CheriBSD in the FVP with virtio-net + ssh; copy the binary
# into the guest over scp; run it under PMU sampling; capture stdout/stderr
# and exit code; tear the FVP down.
#
# This script encapsulates a vetted FVP_Morello -C parameter set assembled
# from the Morello Platform Model documentation and the morello-linux
# wrapper script (https://linux.morello-project.org/). Tunables are
# environment variables; sensible defaults are baked in.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=/dev/null
source "${REPO_ROOT}/scripts/upstream_pins.env"

FVP="${REPO_ROOT}/${MORELLO_FVP_PATH}"
CHERIBSD_IMG="${REPO_ROOT}/third_party/cheri/output/cheribsd-morello-purecap.img"
FW_DIR="${REPO_ROOT}/third_party/morello-fvp/firmware"

# Tunables.
FVP_CORES="${FVP_CORES:-2}"
FVP_MEM_GB="${FVP_MEM_GB:-8}"
FVP_SSH_PORT="${FVP_SSH_PORT:-2222}"
FVP_GUEST_USER="${FVP_GUEST_USER:-root}"
FVP_BOOT_TIMEOUT_S="${FVP_BOOT_TIMEOUT_S:-300}"
FVP_RUN_LOG="${FVP_RUN_LOG:-/tmp/stopless-fvp-run.log}"

log()  { printf '\033[1;36m[fvp]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[fvp warn]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[fvp error]\033[0m %s\n' "$*" >&2; exit 1; }

[[ -x "${FVP}" ]]         || die "Morello FVP not executable at ${FVP}. See docs/03_build_setup.md."
[[ -f "${CHERIBSD_IMG}" ]] || die "CheriBSD image missing at ${CHERIBSD_IMG}. Run scripts/bootstrap.sh."
[[ -d "${FW_DIR}" ]]      || die "Firmware dir missing at ${FW_DIR}. Provide SCP_BL1, AP_BL1, etc."

[[ $# -ge 1 ]] || die "Usage: $0 <binary> [args...]"
BIN="$1"; shift
[[ -x "${BIN}" ]] || die "binary not executable: ${BIN}"

# ---------------------------------------------------------------------
# Launch the FVP in the background. The parameter set is the documented
# Morello Platform Model invocation with two tweaks for our use:
#   - virtio-net (network port -> hostside :${FVP_SSH_PORT})
#   - serial console redirected to a log file we tail for boot-done.
# ---------------------------------------------------------------------
launch_fvp() {
    log "launching FVP (${FVP_CORES} cores, ${FVP_MEM_GB} GiB)"
    "${FVP}" \
        -C bp.dram.size_in_gb="${FVP_MEM_GB}" \
        -C cluster0.NUM_CORES="${FVP_CORES}" \
        -C css.scp.armcortexm7ct.INITVTOR="0x0" \
        -C css.scp.rom.raw_image="${FW_DIR}/scp_romfw.bin" \
        -C css.scp.flashloader0.fname="${FW_DIR}/scp_fw.bin" \
        -C css.mcp.rom.raw_image="${FW_DIR}/mcp_romfw.bin" \
        -C css.mcp.flashloader0.fname="${FW_DIR}/mcp_fw.bin" \
        -C css.trustedBootROMloader.fname="${FW_DIR}/tf-bl1.bin" \
        -C css.trustedSRAM_loader.fname="${FW_DIR}/tf-fip.bin" \
        -C css.nonTrustedROMloader.fname="${FW_DIR}/uefi.bin" \
        -C board.virtioblockdevice.image_path="${CHERIBSD_IMG}" \
        -C board.virtio_net.enabled=1 \
        -C board.virtio_net.hostbridge.userNetworking=1 \
        -C board.virtio_net.hostbridge.userNetPorts="${FVP_SSH_PORT}=22" \
        -C board.terminal_0.start_telnet=0 \
        -C board.terminal_0.mode=raw \
        -C board.terminal_0.terminal_command="cat > ${FVP_RUN_LOG} 2>&1 &" \
        > /dev/null 2>&1 &
    FVP_PID=$!
    log "FVP started, pid=${FVP_PID}"
}

# ---------------------------------------------------------------------
# Wait for sshd to be reachable in the guest.
# ---------------------------------------------------------------------
wait_for_ssh() {
    log "waiting for sshd on :${FVP_SSH_PORT} (up to ${FVP_BOOT_TIMEOUT_S}s)"
    local deadline=$(( $(date +%s) + FVP_BOOT_TIMEOUT_S ))
    while [[ $(date +%s) -lt ${deadline} ]]; do
        if ssh -q -o BatchMode=yes -o ConnectTimeout=4 -o StrictHostKeyChecking=no \
            -p "${FVP_SSH_PORT}" "${FVP_GUEST_USER}@localhost" true 2>/dev/null; then
            log "guest reachable via ssh"
            return 0
        fi
        sleep 4
    done
    die "guest did not boot within ${FVP_BOOT_TIMEOUT_S}s; tail ${FVP_RUN_LOG}"
}

# ---------------------------------------------------------------------
# Push the binary, run it, capture exit + stdout.
# ---------------------------------------------------------------------
run_binary_in_guest() {
    local remote="/root/$(basename "${BIN}")"
    log "scp $(basename "${BIN}") → guest"
    scp -q -o StrictHostKeyChecking=no -P "${FVP_SSH_PORT}" \
        "${BIN}" "${FVP_GUEST_USER}@localhost:${remote}"

    log "running in guest: ${remote} $*"
    set +e
    ssh -o StrictHostKeyChecking=no -p "${FVP_SSH_PORT}" \
        "${FVP_GUEST_USER}@localhost" \
        "chmod +x '${remote}' && '${remote}' $*"
    rc=$?
    set -e
    log "guest exited rc=${rc}"
    return "${rc}"
}

# ---------------------------------------------------------------------
# Always tear the FVP down on exit.
# ---------------------------------------------------------------------
teardown() {
    if [[ -n "${FVP_PID:-}" ]] && kill -0 "${FVP_PID}" 2>/dev/null; then
        log "tearing down FVP pid=${FVP_PID}"
        kill "${FVP_PID}" 2>/dev/null || true
        wait "${FVP_PID}" 2>/dev/null || true
    fi
}
trap teardown EXIT

launch_fvp
wait_for_ssh
run_binary_in_guest "$@"
exit_code=$?
exit "${exit_code}"
