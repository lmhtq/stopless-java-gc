#!/usr/bin/env bash
# scripts/run_in_fvp.sh — run a purecap-aarch64 binary in Morello FVP.
# Cycle-approximate; usable for paper-grade relative performance numbers.
#
# Usage: scripts/run_in_fvp.sh <binary> [args...]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=/dev/null
source "${REPO_ROOT}/scripts/upstream_pins.env"

FVP="${REPO_ROOT}/${MORELLO_FVP_PATH}"
CHERIBSD_IMG="${REPO_ROOT}/third_party/cheri/output/cheribsd-morello-purecap.img"

die() { printf '\033[1;31m[fvp error]\033[0m %s\n' "$*" >&2; exit 1; }

[[ -x "${FVP}" ]] || die "Morello FVP not at ${FVP}. See docs/03_build_setup.md."
[[ -f "${CHERIBSD_IMG}" ]] || die "CheriBSD image missing. Run scripts/bootstrap.sh."

[[ $# -ge 1 ]] || die "Usage: $0 <binary> [args...]"

# TODO(phase0-spike): wire up FVP invocation with the binary and args.
# FVP requires:
#   - SCP_BL1 / AP_BL1 firmware
#   - The CheriBSD image
#   - 9p-mount or virtio-net for getting the binary into the guest
#   - Stdout capture
#
# The Morello FVP getting-started guide documents the exact -C parameters.

cat <<EOF
[run_in_fvp] (placeholder)
   FVP:      ${FVP}
   image:    ${CHERIBSD_IMG}
   binary:   $1
   args:     ${*:2}

Phase 0 spike S1 wires this up. The intended flow:
   1. Boot FVP with CheriBSD image
   2. 9p-mount repo root into guest
   3. ssh into guest, run binary with PMU-counter capture
   4. Tear down FVP, return exit code
EOF
