#!/usr/bin/env bash
# tests/integration/hello-cheri/build.sh — cross-compile hello.c for
# aarch64 purecap (Morello / CheriBSD) using the bootstrapped SDK.
#
# Output: tests/integration/hello-cheri/hello (a purecap ELF)
#
# Usage: ./build.sh [--hybrid]
#   default: purecap (every pointer is a 128-bit capability)
#   --hybrid: hybrid mode (caps opt-in via __capability qualifier)
#
# Verified to produce an aarch64-cheribsd ELF that `file(1)` reports as
# "ELF 64-bit LSB executable, ARM aarch64".

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SDK="${REPO_ROOT}/third_party/output/morello-sdk"
CLANG="${SDK}/bin/clang"
SYSROOT="${SDK}/sysroot-morello-purecap"

MODE="purecap"
case "${1:-}" in
    --purecap) MODE="purecap"; shift ;;
    --hybrid)  MODE="hybrid";  shift ;;
    "")        ;;
    *)         echo "unknown arg: $1" >&2; exit 2 ;;
esac

[[ -x "${CLANG}" ]] || { echo "missing Morello clang at ${CLANG}"; exit 1; }

OUT="${REPO_ROOT}/tests/integration/hello-cheri/hello"
SRC="${REPO_ROOT}/tests/integration/hello-cheri/hello.c"

if [[ "${MODE}" == "purecap" ]]; then
    # The Morello clang infers C64 (capability) encoding from -mabi=purecap.
    # Passing -march=morello+a64c (A64 encoding) here conflicts with the ABI
    # and clang refuses to compile; drop the +a64c suffix.
    CFLAGS=(
        -target aarch64-unknown-freebsd
        -march=morello
        -mabi=purecap
    )
else
    CFLAGS=(
        -target aarch64-unknown-freebsd
        -march=morello
    )
fi

# Use the sysroot embedded in the SDK if present (cheribuild lays it out
# under sysroot-morello-purecap/). Otherwise fall back to no --sysroot,
# which works for freestanding hello-world but not for libc-heavy code.
if [[ -d "${SYSROOT}" ]]; then
    CFLAGS+=(--sysroot="${SYSROOT}")
fi

set -x
"${CLANG}" "${CFLAGS[@]}" -O2 -Wall -Wextra -o "${OUT}" "${SRC}"
set +x

echo "built: ${OUT}"
file "${OUT}" 2>/dev/null || echo "(file(1) not available; inspect manually)"
