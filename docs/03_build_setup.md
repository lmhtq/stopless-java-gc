# Build Setup

End-to-end instructions for getting from a clean checkout to a patched
JDK that runs on Morello FVP. Everything third-party is fetched into
`third_party/` by `scripts/bootstrap.sh`; nothing is committed to this
repo.

## Prerequisites

| Tool | Min version | Why |
|---|---|---|
| macOS or Ubuntu 22.04+ | — | cheribuild supports both |
| Python 3.10+ | — | cheribuild driver |
| Bash 5+ | — | scripts/ |
| Git 2.30+ | — | sparse checkout, ssh-clone |
| CMake 3.20+ | — | tests/ and src/cap_runtime/ |
| Mercurial | — | OpenJDK 17u uses hg-mirror cadence |
| ~80 GB free disk | — | LLVM + OpenJDK + CheriBSD source + builds |
| ~16 GB RAM | — | OpenJDK build is memory-hungry |

On macOS:

```bash
brew install cmake python git mercurial qemu
xcode-select --install
```

On Ubuntu:

```bash
sudo apt-get install -y build-essential cmake python3 python3-pip \
    git autoconf libtool automake flex bison libssl-dev libffi-dev \
    libxml2-dev libtinfo-dev libncurses-dev pkg-config \
    ninja-build texinfo nasm
```

The trailing `ninja-build texinfo nasm` set is required by `cheribuild`
when it builds Morello LLVM. `scripts/bootstrap.sh` preflight will
fail fast if these are missing.

For the Morello FVP, download separately from
[developer.arm.com](https://developer.arm.com/Tools%20and%20Software/Fixed%20Virtual%20Platforms/Morello%20Platform%20FVPs)
(free, requires Arm developer account). Place the extracted FVP at
`third_party/morello-fvp/` (the bootstrap script will refuse to clobber).

## Bootstrap (one-time)

```bash
git clone <this-repo> stopless-java-gc
cd stopless-java-gc
./scripts/bootstrap.sh
```

The script:

1. Clones `cheribuild`, `openjdk/jdk17u`, MOJO's CheriBSD patches,
   `cheribsd`, and the Cornucopia revoke userland into `third_party/`,
   each pinned to the SHAs in `scripts/upstream_pins.env`.
2. Runs `cheribuild morello-llvm` to build the CHERI-aware Clang/LLVM
   toolchain into `third_party/cheri/output/sdk/`.
3. Runs `cheribuild cheribsd-morello-purecap` to produce a CheriBSD
   image for QEMU/FVP.
4. Verifies the FVP at `third_party/morello-fvp/` is invokable.

Expect this to take 2–6 hours on a modern laptop. Subsequent runs are
incremental.

## Apply our patches

```bash
./scripts/apply_patches.sh
```

This is idempotent — if a patch is already applied, it is skipped. Patch
ordering is controlled by `patches/openjdk-jdk17/series`.

The first patch in the series, `0001-cap-runtime-hook.patch`, modifies
the OpenJDK build system to compile and link our `src/cap_runtime/`
code into HotSpot. All other patches are scoped to specific
HotSpot/ZGC files.

## Build the patched JDK

```bash
./scripts/build_jdk.sh           # builds for Morello (default)
./scripts/build_jdk.sh --debug   # slowdebug build with assertions
./scripts/build_jdk.sh --x86     # cross-build for x86_64 (smoke only)
```

Output: `build/jdk-morello/images/jdk/bin/java`.

## Run

In QEMU (functional only, no perf numbers):

```bash
./scripts/run_in_qemu.sh build/jdk-morello/images/jdk/bin/java -version
```

In Morello FVP (cycle-approximate, paper-grade):

```bash
./scripts/run_in_fvp.sh build/jdk-morello/images/jdk/bin/java \
    -XX:+UseZGC -XX:+UnlockExperimentalVMOptions \
    -Xmx2g -jar third_party/dacapo/dacapo-23.11-MR1.jar h2 -n 5
```

## Tests

```bash
./scripts/run_tests.sh           # unit + integration smoke
./scripts/run_tests.sh --unit    # gtest only
./scripts/run_tests.sh --integ   # jtreg + DaCapo smoke only
```

## Benchmarks

```bash
./scripts/run_benchmarks.sh dacapo h2 pmd
./scripts/plot_results.py bench/results/<run-id>
```

## Common issues

| Symptom | Cause | Fix |
|---|---|---|
| `cheribuild: morello-llvm fails` | Out of disk or memory | Need ≥80 GB free, ≥16 GB RAM |
| `apply_patches.sh: patch does not apply` | OpenJDK pin moved | Update `scripts/upstream_pins.env` or rebase the offending patch |
| FVP refuses to launch | EULA not accepted | Run FVP once interactively to accept EULA, then scripts work |
| `LDR Cx` instruction not recognized at build time | LLVM not Morello-aware | Verify `third_party/cheri/output/sdk/bin/clang --version` reports Morello support |
