#!/usr/bin/env bash
# scripts/build_jdk.sh — build the patched OpenJDK 17u for Morello (default),
# or x86_64 / slowdebug for sanity-checking.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JDK="${REPO_ROOT}/third_party/openjdk-jdk17"
CHERI_SDK="${REPO_ROOT}/third_party/output/morello-sdk"
BUILD_ROOT="${REPO_ROOT}/build"

TARGET="morello"
DEBUG_LEVEL="release"

log() { printf '\033[1;36m[build-jdk]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[build-jdk error]\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<EOF
Usage: $0 [--target morello|x86] [--debug|--release|--slowdebug]
EOF
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target)     TARGET="$2";        shift 2 ;;
        --x86)        TARGET="x86";       shift ;;
        --morello)    TARGET="morello";   shift ;;
        --debug)      DEBUG_LEVEL="slowdebug"; shift ;;
        --slowdebug)  DEBUG_LEVEL="slowdebug"; shift ;;
        --release)    DEBUG_LEVEL="release";   shift ;;
        -h|--help)    usage 0 ;;
        *)            die "unknown arg: $1" ;;
    esac
done

[[ -d "${JDK}/.git" ]] || die "OpenJDK not at ${JDK}. Run scripts/bootstrap.sh."

case "${TARGET}" in
    morello)
        [[ -x "${CHERI_SDK}/bin/clang" ]] || die "Morello SDK missing. Run scripts/bootstrap.sh."
        OUT_DIR="${BUILD_ROOT}/jdk-morello"
        CONF_ARGS=(
            --openjdk-target=aarch64-unknown-freebsd
            --with-toolchain-path="${CHERI_SDK}/bin"
            --with-extra-cflags="-mabi=purecap -march=morello+a64c"
            --with-extra-cxxflags="-mabi=purecap -march=morello+a64c"
            --with-debug-level="${DEBUG_LEVEL}"
            --disable-warnings-as-errors
        )
        ;;
    x86)
        OUT_DIR="${BUILD_ROOT}/jdk-x86"
        CONF_ARGS=(
            --with-debug-level="${DEBUG_LEVEL}"
            --disable-warnings-as-errors
        )
        ;;
    *)
        die "unsupported target: ${TARGET}"
        ;;
esac

mkdir -p "${OUT_DIR}"
cd "${JDK}"

if [[ ! -f "${OUT_DIR}/spec.gmk" ]]; then
    log "configure (${TARGET}, ${DEBUG_LEVEL})"
    bash configure \
        --with-conf-name="$(basename "${OUT_DIR}")" \
        "${CONF_ARGS[@]}"
fi

log "make images (${TARGET})"
make CONF_NAME="$(basename "${OUT_DIR}")" images

log "build complete. JDK at ${OUT_DIR}/images/jdk"
