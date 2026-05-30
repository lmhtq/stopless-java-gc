# C-6 L54–L59: klass/method/csp/esp capabilities, and the A64/C64 return boundary

Session of 2026-05-30. Continues the C-6 cap-port cascade. Each layer = one
integer instruction that strips a 16-byte capability's tag (or uses an 8-byte
slot stride where purecap slots are 16), found by gdb-cheri at the fault PC.

Patches: **0118** (L52/L53 rlocals + aload, carried in), **0119** (L54–L57b),
**0120** (L58).

## The advance

Start of session: faulting at the first real method's `_return_register_
finalizer` (load of `this`). End of session: **executed a full static
initializer** — `new Foo; dup; invokespecial <init>; putstatic` — and
**returned** from it. We crossed: load_klass, the const-method metadata
loads, the machine-SP (csp) reconstruction, the expression-stack (esp)
capability round-trip, and the invoke parameter pop.

## Fixes

| L | site | bug | fix |
|---|------|-----|-----|
| L54 | `macroAssembler::load_klass` | Klass* is a full 16B cap at oop+16 (markWord 8 + 8 pad) under -UseCompressedClassPointers; `ldr` strips tag | cap-LDR; also `_return` receiver `this` -> cap_ldr |
| L55 | narrow / remove_activation / return / deopt / native / exception entries | ConstMethod*/Method* (const_offset, max_stack, size_of_parameters, codes_offset) loaded with integer ldr | cap-LDR (cap-ADD for the rbcp codebase) |
| L56 | `generate_return_entry_for` "Restore machine SP" | `ldr initial_sp; integer sub; andr sp,-16` strips csp's tag | reconstruct csp from the initial_sp CAPABILITY: cap-sub a 16-aligned byte delta, install with the cap-add-reg form targeting csp(#31). csp now tagged `[rwRW,0x42482000-0x42682000]` |
| L57 | last_sp / sender_sp frame slots; native param space | esp loaded/stored with integer ldr/str; native space via integer sub + andr sp + mov esp | cap-LDR/cap-STR; native: esp -= params*16 (cap-sub) then csp = esp (cap-mov) |
| L57b | `call_VM_base` | esp (r20) returns from the codecache<->libjvm trampoline DETAGGED (address intact, within the stack cap) — stock relies on it being callee-saved | re-tag `esp = csp + (esp_addr - csp_addr)` right after the #28 rmethod reload. Fixes every resolve call_VM |
| L58 | `generate_return_entry_for` param pop | `add(esp,esp,size,LSL,3)` = integer add (strips tag) + stock x8 word stride; purecap slots are 16B | cap-add + logStackElementSize (x16). With x8, esp drifts 8B per param into mid-slot -> next atos `ldr c0,[esp]` faults SIGBUS/BUS_ADRALN |

L58 was the prettiest: `static f = new Foo()` -> new;dup;invokespecial
<init>;putstatic. After `<init>` (1-slot receiver) the param pop must advance
esp 16 bytes back to the surviving ref; x8 left it 8 high (mid-oop), so the
putstatic atos load BUS_ALN'd. After the fix the putstatic value loads as a
valid tagged oop.

## L59 — the current blocker: A64/C64 mode at the libjvm->codecache entry

After the <clinit>'s `return`, control returns to the JavaCalls **call stub**
(`generate_call_stub`) at the instruction after its `blr x4` (the interpreter
entry call). That instruction (`ldur x3, [c29,#-112]`, result-address load)
faults **SIGBUS / si_code=1 (BUS_ADRALN)**.

Root cause (confirmed by registers at the fault):
- `pcc = 0x428b41c1` — PCC **address has bit0=1**.
- `c30 (lr) = 0x428b41c1 (sentry)` — the return sentry also has bit0=1.

bit0=1 is the **Morello C64 instruction-set marker**. The codecache is **A64**
(HotSpot's aarch64 backend emits A64 + Morello capability instructions). The
method's `ret` landed on an odd PC (0x428b41c1) → instruction fetch from an
odd address faults. The C64 bit was baked into the return sentry by the call
stub's `blr x4` executing with **PSTATE.C64 = 1** — i.e. the call stub was
*entered* from libjvm (compiled purecap = C64) **without a switch back to A64**.

This is the same A64/C64 boundary the **L25 trampoline investigation** flagged
as "needs Morello ISA-level work" (see docs/33), now pinned to a concrete
site: the **libjvm -> codecache** crossing at `JavaCalls -> StubRoutines::
call_stub()`. The codecache<->libjvm calls (cap_blr trampoline) handle the
A64->C64 direction; the reverse (C64 libjvm entering the A64 codecache) does
not clear PSTATE.C64.

### Candidate directions (architectural — needs a decision)
1. Ensure every codecache entry capability reachable from libjvm (call stub
   entry, i2c/c2i, etc.) is an **A64** capability (bit0=0) so entering it
   switches PSTATE.C64←0.
2. Make the interpreter return path explicitly clear the mode bit (force A64)
   before `ret`.
3. (Big hammer) generate the codecache in C64 to match libjvm — avoids all
   mode switches but is a major backend change.

### Also noted (future *8->*16 fixes)
- Call stub param-space alloc `sub x8, sp, w6, uxtw#3` is x8 (should be x16).
- L56 csp reconstruction still uses `lsl #3` (x8) while the entry Move-SP uses
  x16 (logStackElementSize) — latent inconsistency; align to x16 if it bites.

## Reproduce
```
make CONF_CHECK=ignore hotspot-server-libs            # on hasee
strip + scp build/.../libjvm.so -> guest:/opt/jdk/lib/server/
gdb -ex 'catch signal SIGPROT SIGSEGV SIGBUS SIGILL' \
    -ex run -ex 'info registers pc pcc c30 c29' --args \
  /opt/jdk/bin/java -XX:+UnlockExperimentalVMOptions -XX:+UseStoplessGC \
    -Xms16m -Xmx32m -XX:-UseCompressedClassPointers -XX:-UseCompressedOops -version
```
