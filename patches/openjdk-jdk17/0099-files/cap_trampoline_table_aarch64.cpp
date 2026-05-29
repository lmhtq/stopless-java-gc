/*
 * cap_trampoline_table_aarch64.cpp — DATA ONLY translation unit.
 *
 * This TU contains JUST the global table-pointer variable so that all
 * other TUs accessing it must reference it as `extern`, forcing the
 * compiler to emit GOT-style relocation-resolved cap loads rather
 * than PCC-relative ADRP+ADD. The trampoline dispatch and accessor
 * functions live in cap_trampoline_aarch64.cpp; they access this
 * variable via the GOT, which IS within their PCC bounds (libjvm.so
 * CFI bounds covering [.rodata, .got, .pad.cheri.pcc]).
 */

#include "precompiled.hpp"
#include "memory/allocation.hpp"
#include "utilities/growableArray.hpp"

GrowableArray<address>* _cap_tramp_fn_table = nullptr;
