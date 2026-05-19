# third_party/

**This directory is gitignored except for this README and `.gitkeep`.** It is
populated by `scripts/bootstrap.sh` from upstream sources. Nothing here is
under our copyright.

## Expected contents after bootstrap

| Path | Upstream | License | Why |
|---|---|---|---|
| `cheribuild/` | https://github.com/CTSRD-CHERI/cheribuild | BSD-2-Clause | Orchestrates the CHERI toolchain build (clang, QEMU, CheriBSD, OpenJDK target). |
| `openjdk-jdk17/` | https://github.com/openjdk/jdk17u | GPLv2 with Classpath Exception | The JVM we patch. |
| `mojo-patches/` | https://github.com/mojo-jvm/openjdk-cheribsd-patches *(or upstream-released tarball)* | GPLv2-CE | MOJO's Epsilon/Serial/G1 port to CheriBSD on Morello — our baseline. |
| `cheribsd/` | https://github.com/CTSRD-CHERI/cheribsd | BSD-3-Clause | OS image for QEMU / FVP / real Morello. |
| `cornucopia/` | CheriBSD revoke implementation (part of cheribsd repo) | BSD | Phase 2 integrates with this. |
| `morello-fvp/` | https://developer.arm.com (Morello Platform FVP) | Arm EULA, free | Cycle-approximate emulator for performance numbers. |

## Reproducibility

`scripts/bootstrap.sh` pins each upstream to a specific commit SHA / tagged
release. The pin file lives at `scripts/upstream_pins.env`. To update a pin,
edit that file and re-run bootstrap; commit the pin change with the
corresponding patch updates if the upstream interface shifted.
