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

# Auto-detect a boot JDK (must be 16 or 17 for OpenJDK 17u). Honor caller
# override via $BOOT_JDK environment variable.
if [[ -z "${BOOT_JDK:-}" ]]; then
    for cand in /usr/lib/jvm/java-17-openjdk-amd64 \
                /usr/lib/jvm/java-17-openjdk \
                /opt/homebrew/Cellar/openjdk@17/*/libexec/openjdk.jdk/Contents/Home \
                /Library/Java/JavaVirtualMachines/temurin-17*/Contents/Home; do
        if [[ -x "${cand}/bin/javac" ]]; then BOOT_JDK="${cand}"; break; fi
    done
fi
[[ -n "${BOOT_JDK:-}" && -x "${BOOT_JDK}/bin/javac" ]] \
    || die "no JDK 16/17 found; install openjdk-17-jdk-headless or pass BOOT_JDK=<path>"

case "${TARGET}" in
    morello)
        [[ -x "${CHERI_SDK}/bin/clang" ]] || die "Morello SDK missing. Run scripts/bootstrap.sh."
        OUT_DIR="${BUILD_ROOT}/jdk-morello"
        # On the Morello SDK, -mabi=purecap selects c64 encoding; passing
        # +a64c in -march conflicts with the ABI. See
        # tests/integration/hello-cheri/build.sh for the same fix.
        # OpenJDK's autoconf invokes test compiles with `-isysroot SYS`
        # (clang headers only) rather than `--sysroot=SYS` (also libraries),
        # so the link-test fails to find Scrt1.o / libc / libgcc. Force
        # --sysroot= via the extra-flag passthrough; that survives into
        # both compile and link invocations.
        SYS="${CHERI_SDK}/sysroot-morello-purecap"
        CONF_ARGS=(
            --openjdk-target=aarch64-unknown-freebsd
            --with-toolchain-type=clang
            --with-toolchain-path="${CHERI_SDK}/bin"
            --with-sysroot="${SYS}"
            --with-extra-cflags="--sysroot=${SYS} -mabi=purecap -march=morello"
            --with-extra-cxxflags="--sysroot=${SYS} -mabi=purecap -march=morello"
            --with-extra-asflags="--sysroot=${SYS} -mabi=purecap -march=morello"
            --with-extra-ldflags="--sysroot=${SYS} -mabi=purecap"
            --with-boot-jdk="${BOOT_JDK}"
            --with-debug-level="${DEBUG_LEVEL}"
            --disable-warnings-as-errors
            --enable-headless-only
            # Cross-compile fallback: point at host's CUPS / ALSA / fontconfig
            # headers since CheriBSD sysroot doesn't ship them. The Java code
            # paths that use these aren't on our hot path; we mostly need the
            # checks to pass so jdk.print etc. can build as no-op stubs.
            --with-cups-include=/usr/include
            --with-alsa-include=/usr/include
            --with-fontconfig-include=/usr/include
            --with-freetype-include=/usr/include/freetype2
            --with-freetype-lib=/usr/lib/x86_64-linux-gnu
        )
        ;;
    x86)
        OUT_DIR="${BUILD_ROOT}/jdk-x86"
        CONF_ARGS=(
            --with-boot-jdk="${BOOT_JDK}"
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
    # For cross-compilation, OpenJDK needs the build-side compilers to also
    # be clang (it refuses to mix clang target with gcc build). The
    # autoconf wrapper rejects BUILD_CC from the environment but accepts it
    # as a command-line variable.
    HOST_CLANG="$(command -v clang || true)"
    HOST_CLANGXX="$(command -v clang++ || true)"
    bash configure \
        --with-conf-name="$(basename "${OUT_DIR}")" \
        "${CONF_ARGS[@]}" \
        BUILD_CC="${HOST_CLANG}" \
        BUILD_CXX="${HOST_CLANGXX}"
fi

log "make images (${TARGET})"
make CONF_NAME="$(basename "${OUT_DIR}")" images

log "build complete. JDK at ${OUT_DIR}/images/jdk"
