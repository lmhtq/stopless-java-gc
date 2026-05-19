#!/usr/bin/env bash
# scripts/apply_patches.sh — idempotently apply patches/openjdk-jdk17/*.patch
# to third_party/openjdk-jdk17/ in the order given by
# patches/openjdk-jdk17/series.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JDK="${REPO_ROOT}/third_party/openjdk-jdk17"
PATCHES_DIR="${REPO_ROOT}/patches/openjdk-jdk17"
SERIES="${PATCHES_DIR}/series"

log()  { printf '\033[1;36m[apply-patches]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[apply-patches error]\033[0m %s\n' "$*" >&2; exit 1; }

[[ -d "${JDK}/.git" ]] || die "OpenJDK not present at ${JDK}; run scripts/bootstrap.sh first."
[[ -f "${SERIES}"   ]] || die "No patch series at ${SERIES}. Create it or there are no patches yet."

# Use a branch in the OpenJDK checkout to track our patch state. Idempotency:
# we record the SHA of the upstream tip + last-applied patch in
# .stopless/patch-state, and skip already-applied patches.
mkdir -p "${JDK}/.stopless"
STATE="${JDK}/.stopless/patch-state"

upstream_tip="$(git -C "${JDK}" rev-parse HEAD)"
if [[ -f "${STATE}" ]]; then
    # shellcheck source=/dev/null
    source "${STATE}"
    if [[ "${BASE_SHA:-}" != "${upstream_tip}" ]]; then
        die "Upstream HEAD changed (was ${BASE_SHA}, now ${upstream_tip}). Reset OpenJDK or re-pin."
    fi
fi

while IFS= read -r patch_name; do
    [[ -z "${patch_name}" || "${patch_name}" =~ ^# ]] && continue
    patch_path="${PATCHES_DIR}/${patch_name}"
    [[ -f "${patch_path}" ]] || die "missing patch file: ${patch_path}"

    if grep -q "^APPLIED=${patch_name}$" "${STATE}" 2>/dev/null; then
        log "skip (already applied): ${patch_name}"
        continue
    fi

    log "applying: ${patch_name}"
    ( cd "${JDK}" && git apply --check "${patch_path}" ) \
        || die "patch does not apply cleanly: ${patch_name}. See docs/03_build_setup.md → 'Common issues'."
    ( cd "${JDK}" && git apply "${patch_path}" )
    {
        echo "BASE_SHA=${upstream_tip}"
        echo "APPLIED=${patch_name}"
    } >> "${STATE}"
done < "${SERIES}"

log "all patches applied"
