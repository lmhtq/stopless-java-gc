/*
 * cap_trampoline_aarch64.cpp — dispatch + accessor functions.
 *
 * Global table-pointer variable lives in a SEPARATE TU
 * (cap_trampoline_table_aarch64.cpp) so that accesses from this TU are
 * via `extern` declaration, forcing the compiler to emit GOT-style
 * cap loads. Required because our function PCC has narrow CFI bounds
 * = [.rodata, .got, .pad.cheri.pcc] which do NOT cover .bss but DO
 * cover .got — so going via the GOT yields a cap with bounds reaching
 * into .bss.
 *
 * STATUS as of L25 investigation: BLR via stub-loaded trampoline cap
 * faults at the trampoline's first instruction with si_code=1
 * (PROT_CHERI_BOUNDS) even for a NAKED trampoline that does just
 * `mov x0, #0xdead; ret c30`. The cap is a sentry with valid bounds
 * covering the trampoline address. Root cause not yet pinpointed —
 * likely a Morello-specific BLR-mode-switch quirk where the bit-0
 * mode marker in the sentry cap's address interacts with PSTATE.C64
 * in an unexpected way. Needs Morello ISA-level investigation
 * beyond what gdb-cheri alone can answer.
 */

#include "precompiled.hpp"
#include "memory/allocation.hpp"
#include "utilities/growableArray.hpp"

#include <cstdio>
#include <cstdlib>

extern GrowableArray<address>* _cap_tramp_fn_table;

int cap_trampoline_id_for(address fn) {
  if (_cap_tramp_fn_table == nullptr) {
    _cap_tramp_fn_table = new (ResourceObj::C_HEAP, mtInternal)
        GrowableArray<address>(64, mtInternal);
  }
  for (int i = 0; i < _cap_tramp_fn_table->length(); i++) {
    if (_cap_tramp_fn_table->at(i) == fn) return i + 1;
  }
  _cap_tramp_fn_table->append(fn);
  int id = _cap_tramp_fn_table->length();
  // C-6 L36 diag: map fn_id -> runtime fn address so we can identify
  // which call (e.g. fn_id 19) posts a pending exception.
  fprintf(stderr, "[C6TRAMP] fn_id %d -> %p\n", id, (void*)fn);
  fflush(stderr);
  return id;
}

extern "C" intptr_t cap_trampoline_dispatch(int fn_id,
                                            intptr_t a0,
                                            intptr_t a1,
                                            intptr_t a2,
                                            intptr_t a3,
                                            intptr_t a4) {
  if (_cap_tramp_fn_table == nullptr ||
      fn_id < 1 || fn_id > _cap_tramp_fn_table->length()) {
    fprintf(stderr, "[cap_trampoline] bad fn_id=%d\n", fn_id);
    fflush(stderr);
    abort();
  }
  typedef intptr_t (*FnType)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
  FnType f = (FnType)_cap_tramp_fn_table->at(fn_id - 1);
  // C-6 L36 diag: log the resolved fn cap for low fn_ids (exception path
  // is fn_id 1) or any tag-stripped cap. Non-tail (store r) so the log
  // is meaningful and we can spot a corrupt f before the call.
  if (fn_id <= 2 || !__builtin_cheri_tag_get((void*)f)) {
    fprintf(stderr, "[C6DISP] fn_id=%d f=%#p tag=%d a0=%#lx a1=%#lx\n",
            fn_id, (void*)f, (int)__builtin_cheri_tag_get((void*)f),
            (unsigned long)a0, (unsigned long)a1);
    fflush(stderr);
  }
  intptr_t r = f(a0, a1, a2, a3, a4);
  return r;
}
