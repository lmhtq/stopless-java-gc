# Phase task tracker

Persistent dump of the Claude TaskList state at end of Mac session
2026-05-27. Next Claude session should reconstruct these as live
tasks using `TaskCreate` so it can `TaskUpdate` them as work
progresses.

## Live task list at handoff

| # | Phase | Status | Notes |
|---|---|---|---|
| 25 | **C-1**: StoplessGC HotSpot 骨架 | ✅ completed | patch 0080 canonical, applies clean |
| 26 | **C-2**: -XX:+UseStoplessGC build 接入 | ✅ completed | patch 0081 canonical (8 files), applies clean |
| 27 | **C-3**: StoplessArena C++ 包装 | 🟡 design+impl done; patch needs regen | docs/c3 complete; 0083 hand-authored, won't apply |
| 28 | **C-4**: StoplessAllocator (bump-pointer + csetbounds) | 🟡 cap_runtime done + tested; JVM wire-up patch needs regen | C side passes test_alloc + test_alloc_concurrent on QEMU; 0085 hand-authored |
| 29 | **C-5**: Hello-World — `java -XX:+UseStoplessGC -version` | ⬜ pending (blocked by #37 + needs C-3/C-4 patches landed + C-6) | currently SIGPROT during VM init |
| 30 | **C-6**: Phase 1 shift=64 sbfm/ubfm bug 收尾 | ⬜ pending (blocked by #37) | aarch64 encoder shift overflow during early init |
| 31 | **C-7**: SIGPROT handler 安装 + cap_runtime 桥接 | ⬜ pending | install via JVM crash handler protocol |
| 32 | **C-8**: Root scanner (JavaThread stack + statics + JNI) | ⬜ pending | precise root scan on purecap stack |
| 33 | **C-9**: StoplessCollectorThread (并发 mover) | ⬜ pending | the actual moving GC loop |
| 34 | **C-10**: 写屏障 (concurrent move) | ⬜ pending | mprotect-based or trap-driven |
| 35 | **C-11**: 稳定性 + Microbench | ⬜ pending | paper §5 evaluation core |
| 36 | **C-12**: DaCapo subset + Paper draft | ⬜ pending | lusearch / fop / h2 + paper §6 |
| 37 | hasee 验证 C-1..C-4 (`c_phase_verify.sh`) | ✅ completed | Stage A pass; B/C/D issues documented |

## Next-step priority order

1. **#27/#28 patch regen** (use `docs/20_c3_c4_regen_playbook.md`)
   — gets C-3+C-4 patches canonical and applied on hasee. Until
   this happens, `mem_allocate` returns nullptr, so any allocation
   attempt fails. Estimate: 30-45 min on hasee.

2. **#30 C-6 shift=64** — required to get past early VM init.
   Estimate: 1-3 days of bisect work (Phase 1 style).

3. **#29 C-5 acceptance** — `java -XX:+UseStoplessGC -version`
   exits 0. Unblocks evaluation harness.

4. **#31 C-7 SIGPROT** — required before #33 collector can fire.

5. **#32 C-8 root scanner** — must precede #33 for any correctness.

6. **#33 C-9 collector** — the big payoff phase.

## Task descriptions (for reconstruction)

### #25 — Phase C-1: StoplessGC HotSpot 骨架
Add minimum-viable CollectedHeap / GCArguments / BarrierSet skeleton
under src/hotspot/share/gc/stopless/. Compiles + links into libjvm.so.
COMPLETED: patch 0080 canonical (417 lines incl header), applies
clean on hasee tree.

### #26 — Phase C-2: -XX:+UseStoplessGC build 接入
Extend OpenJDK enums + arg parser + build system so
`-XX:+UseStoplessGC` is a valid flag selecting StoplessHeap.
COMPLETED: patch 0081 canonical (205 lines), 8 files modified,
applies clean. VM identifies as "stopless gc" in version banner.

### #27 — Phase C-3: StoplessArena C++ 包装
RAII wrapper over cap_runtime's stopless_arena_t. Replaces the
placeholder _arena_capacity field in StoplessHeap with a real
arena that mmap's CHERI-tagged memory and acquires shadow bitmap.
DESIGN+IMPL+TEST docs done (docs/c3/). Patch 0083 hand-authored,
needs regen via docs/20 playbook.

### #28 — Phase C-4: StoplessAllocator (bump-pointer + csetbounds)
Implement lock-free CAS bump allocator on C side (cap_runtime/
allocator.c). Wire it through StoplessArena::allocate. Each
returned cap has bounds set to requested size + CHERI_PERM_SW_VMEM
stripped. CAP_RUNTIME DONE + TESTED (test_alloc, test_alloc_
concurrent both pass). Patch 0085 hand-authored, needs regen.

### #29 — Phase C-5: Hello-World — `java -XX:+UseStoplessGC -version`
Acceptance: JVM starts, prints version banner, exits 0.
Blocked by: #27, #28 patches actually landing + #30 C-6.

### #30 — Phase C-6: Phase 1 shift=64 sbfm/ubfm bug 收尾
Multiple wordSize-misuse sites in aarch64 codegen cause the sbfm/
ubfm encoder shift field to overflow during early init. Was
paused at end of Phase 1; now back on critical path.
Strategy: instrument like patches 0068-0070 did. Bisect to find
which call site triggers it.

### #31 — Phase C-7: SIGPROT handler 安装 + cap_runtime 桥接
StoplessHeap::initialize_serviceability calls
stopless_handler_install(). Hook into JVM crash handler protocol so
SIGPROT (signal 34) routes to our handler before
hs_err generation.

### #32 — Phase C-8: Root scanner (JavaThread stack + statics + JNI)
Walk every JavaThread's stack frames (frame::oops_do under purecap
C64), system dictionary statics, JNI globals. Add discovered oops
to a root set the collector can consult.

### #33 — Phase C-9: StoplessCollectorThread (并发 mover)
ConcurrentGCThread subclass: loop forever, take short STW for root
scan, then concurrently relocate batch of objects to a fresh arena,
forward_table_insert + cheri_revoke. This is the actual moving GC.

### #34 — Phase C-10: 写屏障 (concurrent move)
When mutator writes obj.field=p and target region is evacuating,
need to either block or auto-forward p to new_cap. Simplest:
mprotect src arena pages RO during evac, take SIGSEGV through
existing handler.

### #35 — Phase C-11: 稳定性 + Microbench
Paper §5 microbench: GC trigger frequency × object size × alloc
rate sweep. Record GC pause time (target ≈0), throughput, fault
rate. Per-load cycle counts on QEMU.

### #36 — Phase C-12: DaCapo subset + Paper draft
DaCapo lusearch / fop / h2 in StoplessGC. Paper §5+§6 written.

### #37 — hasee 验证 C-1..C-4 (c_phase_verify.sh)
COMPLETED. Stage A 4/4 pass on QEMU. B/C/D exposed the patch-
authoring divergence issue that led to the regen workflow being
established.

## Dependencies graph

```
#37 (verify) ──┐
               ├──> #29 (C-5 hello)
#27 patch ─────┤             │
#28 patch ─────┤             │
               │             ├──> #31 (C-7 SIGPROT install)
               └──> #30 (C-6 shift=64)
                                     │
                                     ├──> #32 (C-8 roots)
                                     │             │
                                     │             └──> #33 (C-9 collector) ──> #34 (C-10 barrier)
                                                                                       │
                                                                                       └──> #35 (C-11 microbench) ──> #36 (C-12 paper)
```

## How next Claude session should use this

```
1. Read docs/18_handoff_to_hasee_session.md
2. Read this file (docs/22_phase_task_tracker.md)
3. Use TaskCreate to reconstruct tasks #27-#36 (skip completed ones
   unless you want them as audit trail)
4. Set up the dependency graph via TaskUpdate addBlockedBy
5. Pick up at #27/#28 regen per docs/20_c3_c4_regen_playbook.md
```
