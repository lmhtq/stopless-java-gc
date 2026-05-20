# hello-cheri smoke test

Minimal CHERI purecap binary used to verify the bootstrapped Morello
SDK is functional. Three things this catches:

1. Morello clang is on PATH and accepts `-mabi=purecap`.
2. The Morello sysroot ships `<stdio.h>` and the CHERI intrinsics
   header (`<cheriintrin.h>`).
3. The linker produces a valid aarch64-cheribsd ELF.

## Build (on bc@hasee)

```bash
./build.sh           # purecap mode (default)
./build.sh --hybrid  # hybrid mode (caps opt-in)
file hello           # should report aarch64 ELF
```

## Run (optional)

To actually execute the binary, copy it into a running CheriBSD-in-FVP
guest and run it. Expected output (purecap):

```
hello CHERI from stopless-java-gc smoke test
argv0 cap: base=0x... len=0x... perms=0x... tag=1
```

If `tag=1` we've confirmed the loader handed us a tagged capability.
This is the first place where we'd see Phase 1's side-table approach
need to integrate — every Java reference will end up as a similar
tagged cap, and our cap_runtime will mediate access.
