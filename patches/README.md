# patches/

Patches against third-party source. Applied idempotently by
`scripts/apply_patches.sh` after `scripts/bootstrap.sh` clones the upstreams.

## Layout

```
patches/
└── openjdk-jdk17/
    ├── 0001-cap-runtime-hook.patch        # links src/cap_runtime/ into HotSpot build
    ├── 0002-zgc-buildable-on-cheribsd.patch
    ├── 0003-zgc-colored-pointer-to-side-table.patch
    ├── 0004-zgc-multimap-to-cap-derivation.patch
    ├── ...
    └── series                              # `quilt`-style ordering
```

## Discipline

- **One patch = one logical change.** Avoid mega-patches; they obscure
  contribution boundaries and break under upstream rebases.
- **Each patch carries a docs/ ADR reference** in its header (e.g.
  `Refs: docs/01_phase_i_zgc_port.md §3.2`).
- **No third-party source is committed** in patch form except the diff hunks
  themselves. Apply produces real source; that source is build output, not
  versioned content.
- **Generator scripts** (e.g. `tools/regen-patches.sh`) can re-derive patches
  from a clean checkout if needed.

## Net-new vs in-place

| Kind of change | Where it lives |
|---|---|
| Brand-new C++ files (cap-aware runtime, forwarding helpers, side-table) | `src/cap_runtime/` in this repo; linked in via `0001-cap-runtime-hook.patch` |
| Modifications to existing HotSpot files (e.g. `zBarrierSet.cpp`) | A `.patch` in this directory |
| New cpu/morello/gc/z/ files | A patch that adds them (since their location is fixed by HotSpot's source layout) |

The rule: prefer net-new code in `src/cap_runtime/` whenever possible. Only
patch existing files when HotSpot's structure forces it.
