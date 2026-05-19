#!/usr/bin/env bash
# scripts/bootstrap.sh — one-time setup: clone upstreams, build CHERI toolchain,
# build CheriBSD image. Idempotent: safe to re-run after a fresh clone or after
# a pin bump.
#
# Modes:
#   (default)    full bootstrap: preflight + clone all + build Morello SDK
#                (~2–6 hours total; the SDK build dominates)
#   --no-build   preflight + clone only; skip the SDK build. Useful for dev
#                iteration where source analysis is enough.
#   --check      preflight only; report which steps would run.
#   --skip-mojo  skip MOJO patches download (they're an empty URL placeholder
#                until MOJO publishes a stable patch-series tarball).
#
# See docs/03_build_setup.md for prerequisites and expected runtime.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY="${REPO_ROOT}/third_party"

# shellcheck source=/dev/null
source "${REPO_ROOT}/scripts/upstream_pins.env"

MODE_BUILD=1
MODE_CLONE=1
SKIP_MOJO=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build)  MODE_BUILD=0; shift ;;
        --check)     MODE_BUILD=0; MODE_CLONE=0; shift ;;
        --skip-mojo) SKIP_MOJO=1; shift ;;
        -h|--help)
            sed -n '2,16p' "$0"; exit 0 ;;
        *) printf 'bootstrap.sh: unknown arg: %s\n' "$1" >&2; exit 2 ;;
    esac
done

log()  { printf '\033[1;36m[bootstrap]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[bootstrap warn]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[bootstrap error]\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Preflight: dependency check
# ---------------------------------------------------------------------------
preflight() {
    log "preflight: checking host dependencies"
    local missing=()
    for cmd in git python3 cmake bash sha256sum curl unzip tar; do
        command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
    done
    # macOS has shasum, not sha256sum
    if ! command -v sha256sum >/dev/null 2>&1 && command -v shasum >/dev/null 2>&1; then
        missing=("${missing[@]/sha256sum/}")
    fi
    if [[ ${#missing[@]} -gt 0 ]]; then
        die "missing host tools: ${missing[*]}. See docs/03_build_setup.md."
    fi

    local free_gb
    free_gb=$(df -BG "${REPO_ROOT}" 2>/dev/null | awk 'NR==2 {gsub("G",""); print $4}' || echo 0)
    if [[ "${free_gb}" -gt 0 && "${free_gb}" -lt 80 ]]; then
        warn "free disk on $(df -h "${REPO_ROOT}" | awk 'NR==2 {print $6}') is ${free_gb}GB; recommended ≥80GB"
    fi
}

# ---------------------------------------------------------------------------
# Helper: idempotent clone at a given ref
# ---------------------------------------------------------------------------
clone_at_ref() {
    local repo="$1" dir="$2" ref="$3"
    if [[ -d "${dir}/.git" ]]; then
        log "already cloned: ${dir}; fetching ${ref}"
        git -C "${dir}" fetch --quiet origin
    else
        log "cloning ${repo} → ${dir} @ ${ref}"
        git clone --quiet "${repo}" "${dir}"
    fi
    git -C "${dir}" checkout --quiet "${ref}"
}

# ---------------------------------------------------------------------------
# Step 1: cheribuild
# ---------------------------------------------------------------------------
fetch_cheribuild() {
    log "step 1/5: cheribuild"
    mkdir -p "${THIRD_PARTY}"
    clone_at_ref "${CHERIBUILD_REPO}" "${THIRD_PARTY}/cheribuild" "${CHERIBUILD_REF}"
}

# ---------------------------------------------------------------------------
# Step 2: OpenJDK 17u
# ---------------------------------------------------------------------------
fetch_openjdk() {
    log "step 2/5: OpenJDK 17u (${OPENJDK_REF})"
    clone_at_ref "${OPENJDK_REPO}" "${THIRD_PARTY}/openjdk-jdk17" "${OPENJDK_REF}"
}

# ---------------------------------------------------------------------------
# Step 3: MOJO patches
# ---------------------------------------------------------------------------
fetch_mojo() {
    log "step 3/5: MOJO patch series"
    local mojo_dir="${THIRD_PARTY}/mojo-patches"
    mkdir -p "${mojo_dir}"
    if [[ "${SKIP_MOJO}" -eq 1 ]]; then
        warn "--skip-mojo set; will need manual placement at ${mojo_dir}"
        return
    fi
    if [[ -f "${mojo_dir}/.fetched" ]]; then
        log "MOJO patches already fetched; skipping"
        return
    fi
    if [[ -z "${MOJO_PATCHES_URL}" ]]; then
        warn "MOJO_PATCHES_URL is empty; will need manual placement at ${mojo_dir}"
        return
    fi
    log "downloading ${MOJO_PATCHES_URL}"
    if ! curl -fL -o "${mojo_dir}/patches.tgz" "${MOJO_PATCHES_URL}" 2>&1; then
        warn "MOJO download failed (URL may have moved or be a placeholder)."
        warn "Get patches manually from https://www.mojo-jvm.org/ and unpack into ${mojo_dir}/."
        warn "Or re-run with --skip-mojo to silence this step."
        return
    fi
    tar -xzf "${mojo_dir}/patches.tgz" -C "${mojo_dir}"
    touch "${mojo_dir}/.fetched"
}

# ---------------------------------------------------------------------------
# Step 4: CheriBSD source (includes Cornucopia revoke)
# ---------------------------------------------------------------------------
fetch_cheribsd() {
    log "step 4/5: CheriBSD (${CHERIBSD_REF})"
    clone_at_ref "${CHERIBSD_REPO}" "${THIRD_PARTY}/cheribsd" "${CHERIBSD_REF}"
}

# ---------------------------------------------------------------------------
# Step 5: build Morello SDK via cheribuild
# ---------------------------------------------------------------------------
build_morello_sdk() {
    log "step 5/5: building Morello SDK (clang/LLVM + CheriBSD)"
    if [[ -x "${THIRD_PARTY}/cheri/output/sdk/bin/clang" ]]; then
        log "SDK already built; skipping"
        return
    fi
    cd "${THIRD_PARTY}/cheribuild"
    # Source root is one level up (third_party/); cheribuild lays everything
    # out under <source-root>/cheri/...
    python3 cheribuild.py --source-root "${THIRD_PARTY}" \
        morello-llvm \
        --enable-hybrid-targets
    python3 cheribuild.py --source-root "${THIRD_PARTY}" \
        cheribsd-morello-purecap \
        --enable-hybrid-targets
}

verify_fvp() {
    if [[ ! -x "${MORELLO_FVP_PATH}" ]]; then
        warn "Morello FVP not found at ${MORELLO_FVP_PATH}."
        warn "Download from developer.arm.com (free, EULA) and unpack into third_party/morello-fvp/."
        warn "This is not fatal for build, only for run-on-FVP scripts."
    fi
}

final_summary() {
    log ""
    log "=== bootstrap summary ==="
    for d in cheribuild openjdk-jdk17 cheribsd mojo-patches; do
        if [[ -d "${THIRD_PARTY}/${d}" ]]; then
            log "  third_party/${d}                    present"
        else
            log "  third_party/${d}                    MISSING"
        fi
    done
    if [[ -x "${THIRD_PARTY}/cheri/output/sdk/bin/clang" ]]; then
        log "  third_party/cheri/output/sdk/clang  present"
    else
        log "  third_party/cheri/output/sdk/clang  not yet built"
    fi
    if [[ -x "${REPO_ROOT}/${MORELLO_FVP_PATH}" ]]; then
        log "  Morello FVP                         present"
    else
        log "  Morello FVP                         missing (manual download)"
    fi
    log ""
    if [[ "${MODE_BUILD}" -eq 1 ]]; then
        log "Next: ./scripts/apply_patches.sh (no-op until patch series populates)"
    else
        log "Skipped SDK build (--no-build / --check). Re-run without flags to complete."
    fi
}

main() {
    preflight

    if [[ "${MODE_CLONE}" -eq 1 ]]; then
        fetch_cheribuild
        fetch_openjdk
        fetch_mojo
        fetch_cheribsd
    else
        log "--check: would clone cheribuild, openjdk-jdk17, cheribsd, mojo-patches"
    fi

    if [[ "${MODE_BUILD}" -eq 1 ]]; then
        build_morello_sdk
    else
        log "skipping Morello SDK build (--no-build / --check)"
    fi

    verify_fvp
    final_summary
}

main "$@"
