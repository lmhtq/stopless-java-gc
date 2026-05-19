#!/usr/bin/env bash
# scripts/run_in_qemu.sh — run a purecap-aarch64 binary in CHERI-QEMU.
# Functional only; do not use for performance numbers.
#
# Usage: scripts/run_in_qemu.sh <binary> [args...]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU="${REPO_ROOT}/third_party/cheri/output/sdk/bin/qemu-system-morello-purecap"
KERNEL="${REPO_ROOT}/third_party/cheri/output/cheribsd-morello-purecap/boot/kernel/kernel"
IMG="${REPO_ROOT}/third_party/cheri/output/cheribsd-morello-purecap.img"

die() { printf '\033[1;31m[qemu error]\033[0m %s\n' "$*" >&2; exit 1; }

[[ -x "${QEMU}"   ]] || die "QEMU not built at ${QEMU}. Run scripts/bootstrap.sh."
[[ -f "${KERNEL}" ]] || die "Kernel not built at ${KERNEL}. Run scripts/bootstrap.sh."
[[ -f "${IMG}"    ]] || die "Image not built at ${IMG}. Run scripts/bootstrap.sh."

[[ $# -ge 1 ]] || die "Usage: $0 <binary> [args...]"

# TODO(phase0-spike): wire up 9p-share of REPO_ROOT into guest, launch
# binary inside guest. For now, emit a placeholder that documents the
# intended invocation so the harness contract is visible.

cat <<EOF
[run_in_qemu] (placeholder)
   QEMU:    ${QEMU}
   kernel:  ${KERNEL}
   image:   ${IMG}
   binary:  $1
   args:    ${*:2}

Phase 0 spike S1 will wire this up. The intended flow:
   1. Launch QEMU with -kernel \${KERNEL} -drive \${IMG} -nographic
   2. 9p-share repo root into guest
   3. ssh into guest, run binary
EOF
