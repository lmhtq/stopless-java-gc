# Phase 1 Results: OpenJDK 17u on CheriBSD Morello Purecap

Status as of 2026-05-24. This document records the empirical state reached
after the 62-patch porting series for the stopless-java-gc Phase 1 effort
("Port ZGC-capable OpenJDK 17u to ARM Morello CHERI/purecap").

## TL;DR

- `make images` succeeds: produces a CHERI purecap aarch64 JDK image.
- Image binaries are verified ELF: `Flags: 0x10000, purecap`,
  `interpreter /libexec/ld-elf.so.1`, FreeBSD 15 / C64 CheriABI.
- JVM boots through launcher, dynamic linker, libjvm, boot class path,
  safepoint polling page, card table, heap reservation, Epsilon GC init,
  TLAB sizing, StubRoutines generation, and Genesis.
- Next blocker: object initialization (`stp c1, c20, [c0]` — store-pair
  capability) faults because object allocation alignment does not yet
  accommodate capability-sized fields. This is the natural entry point
  for Phase 2 (`oop` as capability).

The 62-patch series and the QEMU-CHERI smoke-test path described here
provide the substrate the Phase 2 ZGC barrier redesign builds on.

## Setup

- **Host**: bc@hasee (x86_64 Ubuntu 22.04, llvm-15)
- **Target toolchain**: cheribuild-built morello-sdk (clang +
  morello-aarch64-purecap-freebsd)
- **Target sysroot**: cheribsd-morello-purecap (CheriBSD 15.0-CURRENT
  built 2026-05-20)
- **Emulator**: CHERI-QEMU (`qemu-system-morello`, built via
  `cheribuild.py qemu`, 64 MB binary at
  `~/cheri/output/sdk/bin/qemu-system-morello`)
- **Bootstrap JDK**: openjdk-17-jdk-headless on host
- **Build target**: `--openjdk-target=aarch64-unknown-freebsd --with-toolchain-type=clang
  --enable-headless-only --with-jvm-features=-compiler2`

The FVP (`FVP_Morello_0.11_34`) was attempted three times early in
deployment but proved unstable: two attempts crashed the FVP itself
with SIGSEGV during guest sshd startup, one hit a guest virtio-blk
kernel panic. QEMU-CHERI boots reliably in ~1 minute and is the
recommended path for ongoing bring-up.

## The patch series

62 patches organized in five conceptual bands. Counts approximate.

### Band 1 — build system (patches 0002-0023, 22 patches)
HotSpot / OpenJDK build infrastructure that didn't anticipate the BSD
target combined with the CHERI/purecap ABI. Notable:

- `platform.m4` accept `cheri-cap` pointer-size triple
- `JvmMapfile.gmk` BSD path
- `CFLAGS_OS_DEF_JVM` carry `-D_ALLBSD_SOURCE` + `-DBSD`
- `bytes_bsd_aarch64.hpp` use `<sys/endian.h>` not `<endian.h>`
- `bitmap` / `shenandoah_bitmap` `bm_word_t` typedef to `uint64_t`
- `BUILD`-vs-`TARGET` pass JVM macro re-derivation (0009)
- aarch64 assembler `mov(Register, intptr_t)` + `uintptr_t` overloads under purecap
- count_trailing_zeros / count_leading_zeros CHERI overloads
- `EXTRA_ASFLAGS` override in `buildjdk-spec.gmk.in`
- aarch64 SafeFetchN intptr_t-vs-int64_t under purecap
- `frame_aarch64.cpp` -O0 wrapper to dodge clang ICE
- pauth bsd_aarch64 cap pass-through
- `globalDefinitions` intx bounds constexpr fixes

### Band 2 — HotSpot/BSD adapters (patches 0024-0036, 13 patches)
HotSpot's BSD os-layer was largely written for Darwin. The non-Apple
BSD branch needed substantial filling-in:

- `mov(Register, uintptr_t)` overload under CHERI
- `metaspaceShared.cpp` use `bm_word_t*`
- `oopStorage` `_active_index` plain volatile load (no SafeFetchN)
- `os_perf_bsd.cpp` stub network_utilization on non-Apple BSD
- `os_bsd_aarch64.cpp` mcontext accessors for CheriBSD purecap (cap_x, cap_elr, cap_sp)
- `os_bsd.cpp` FreeBSD fixups: const error_report, AARCH64 to arch_array,
  struct link_map, HW_CPU_FREQ wrap, xsw_usage wrap
- posix/bsd non-Apple aarch64 fixups (W^X, MAP_NORESERVE, os::Linux)
- aarch64 `SPELL_REG_SP` → `csp`, `SPELL_REG_FP` → `c29` under purecap
- `psScavenge` MIN2 explicit size_t cast
- `Modules.gmk` java.base borrows macosx/classes (per-module override)
- shenandoah passive heuristics MAX2 cast
- `vm_version_bsd_aarch64.cpp` CheriBSD stub for get_os_cpu_info

### Band 3 — BUILDJDK pass host-Linux vs target-CheriBSD CFLAGS (patches 0037-0043, 7 patches)
The OpenJDK cross-build uses a BUILDJDK pass that runs host x86 Linux
clang to produce bootstrap tools. The TARGET spec's CFLAGS_JDKLIB
gets baked at configure time and leaks BSD-specific defines into the
host pass:

- `flags-cflags.m4` `_GNU_SOURCE` + `_DEFAULT_SOURCE` for JDK on bsd
- `flags-cflags.m4` `-Wl,--undefined-version` for bsd target (LLD)
- java.desktop X11 wrapper gen on bsd
- UnixConstants.java.template `__linux__` priority over `_ALLBSD_SOURCE`
- charsets `stdcs-bsd` aliases to `stdcs-linux`
- fdlibm endian `__linux__` priority
- `buildjdk-spec.gmk.in` `$(filter-out)` `_ALLBSD_SOURCE / -DBSD` from BUILD-pass JDK CFLAGS

### Band 4 — launcher and TARGET libs (patches 0044-0052, 9 patches)
The TARGET cross-compile native libraries:

- `LauncherCommon.gmk` LIBS_bsd + LDFLAGS_bsd
- TimeZone_md.c bsd uses linux/macosx code path
- jli_util.h JLI_Lseek for bsd
- net_util_md.h explicit `<netinet/in.h>`
- FileDispatcherImpl statvfs64 bsd as macosx
- libnio bsd: IPv6 alias, UnixCopyFile macosx-only, UnixNativeDispatcher xattr
- mlib_image_types.h CHERI sizeof(void*)=16 reserved[]
- skip libawt + libattach native on bsd
- skip jdk.sctp + libmanagement_ext native on bsd

### Band 5 — runtime fixes from actual JVM execution (patches 0053-0062, 10 patches)
With `make images` succeeding, deploying to a CheriBSD QEMU guest and
running `/opt/jdk/bin/java -version` exposed runtime issues:

- 0053: os_bsd.cpp three CheriBSD/purecap mmap+path fixes (java_home strip; pd_commit_memory mprotect; anon_mmap PROT_MAX)
- 0054: copy_bsd_aarch64.hpp pd_conjoint_words CHERI memmove
- 0055: new os/bsd/decoder_bsd_elf.cpp for non-Apple ElfDecoder::demangle
- 0056: universe.cpp temporarily skip LogHeapWordSize guarantee (made redundant by 0060)
- 0057: jvmFlagConstraintsGC.cpp defer TLAB constraint when max_size not yet initialized
- 0058: assembler_aarch64.hpp SIMD ld_st imm mismatch → warn (diagnostic)
- 0059: aarch64 SIMD post-index stride uses `BytesPerLong` not `wordSize` (5 sites in macroAssembler + 2 in c1_Runtime1)
- 0060: **HeapWord / MetaWord = 8-byte struct under CHERI purecap** (the central refactor)
- 0061: BinList smallest_word_size bumped to 4 under purecap (Block now 32 B)
- 0062: genCollectedHeap skip `HeapWordSize == wordSize` guarantee (intentionally violated on CHERI)

## JVM bring-up depth reached

Running `/opt/jdk/bin/java -XX:+UnlockExperimentalVMOptions -XX:+UseEpsilonGC -Xms32m -Xmx64m -XX:-UseCompressedOops -XX:-UseCompressedClassPointers -version`
inside CheriBSD QEMU progresses through:

1. **`libjli` launcher** loads, parses command line.
2. **`libjvm.so`** loads (53 deferred symbols resolved via `decoder_bsd_elf.cpp`, copy fallbacks via 0054, etc.).
3. **Boot class path** resolved (`/opt/jdk/lib/modules` found via 0053's java_home strip).
4. **Safepoint polling page** committed (mprotect path via 0053).
5. **Card table** committed (same path).
6. **Heap reserved** (post-0060 sizeof(HeapWord) = 8).
7. **Epsilon GC initialized** (`Java VM: ... epsilon gc, bsd-aarch64`).
8. **TLAB sizing constraints** pass (0057).
9. **StubRoutines generation 1** completes (post-0059 SIMD stride fix).
10. **Genesis** (class initialization start) begins.
11. **Object initialization faults** at `stp c1, c20, [c0]`. The
    capability `c0` points to a buffer too small (or insufficiently
    aligned) to hold a pair of 16-byte capabilities. This is where
    Phase 2's oop-as-capability work begins.

The truss-level signal is `SIGBUS (0xa)` at `libjvm.so+0x541ef0`.
Disassembly shows the failing instruction is a `stp` (store pair
capability) emitted by a HotSpot-internal struct constructor.

## Open issues (Phase 2 entry points)

1. **oop = capability redesign**. `oopDesc` and `Klass*` storage must
   accommodate 16-byte capability fields with proper alignment.
   `markWord` ordering and `Klass*` placement in headers must be
   revisited. This intersects directly with ZGC's load barrier (the
   colored-pointer bits) and is the natural locus for the Phase 2
   contribution (capability tag in place of color bits).

2. **`mprotect(0xfc000000, ...)` for compressed class space**. The
   JVM tries to map class metaspace at a specific virtual address for
   compressed-klass-pointer optimization; on CheriBSD this fails because
   the integer address has no associated capability. With
   `-XX:-UseCompressedClassPointers` we work around this, but a proper
   fix requires producing a capability that spans the desired address
   range (probably by reserving a large region and mprotecting the
   subset).

3. **ZGC enablement**. Build currently has `-XX:+UseZGC` disabled
   because `--with-jvm-features=-compiler2` (the clang ICE workaround)
   cascades and removes ZGC. Re-enabling ZGC will require either
   reintroducing C2 selectively or excising the C2 dependency from
   ZGC's load-barrier emit path.

4. **HotSpot HeapWord refactor downstream conflations** (referenced
   by 0062's comment). Many code paths assume `HeapWordSize ==
   wordSize`; the conflations are tracked in 01 §4.2 and addressed as
   they surface during further JVM execution.

## Reproducing the current state

```bash
# Patches apply cleanly against jdk-17.0.13-ga
cd third_party/openjdk-jdk17 && bash scripts/apply_patches.sh

# Build (~30 min from clean, much less incremental)
bash scripts/build_jdk.sh

# Boot QEMU CHERI guest
cheribuild.py run-fvp-morello-purecap   # actually uses qemu-system-morello

# Inside the guest
scp -P 10005 build/jdk-morello/images/jdk root@localhost:/opt/
ssh -p 10005 root@localhost \
  /opt/jdk/bin/java -XX:+UseEpsilonGC -Xms64m -Xmx64m -version
```

Build artifacts live at
`third_party/openjdk-jdk17/build/jdk-morello/images/jdk/`. The `bin/java`
ELF reports `purecap` in its e_flags, and `lib/server/libjvm.so` is a
CheriBSD purecap shared object.

## Counted by hand

- 62 patches in `patches/openjdk-jdk17/series`
- ~70 build attempts on hasee, build36 → build73
- 15 commits in `main`
- Two days wall-clock (2026-05-21 / 2026-05-22) for the build-passes-but-doesnt-run state, plus one day for runtime bring-up + HeapWord refactor.
