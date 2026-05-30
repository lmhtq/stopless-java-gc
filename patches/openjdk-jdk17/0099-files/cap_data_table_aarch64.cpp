/*
 * cap_data_table_aarch64.cpp — C-6 general "libjvm.so data capability" table.
 *
 * Generated interpreter/stub code in the codecache cannot reach libjvm.so
 * .data/.bss globals: `mov reg, &global` materialises an integer (tag-0),
 * PCC-relative adrp is out of the codecache PCC bounds, and DDC is null in
 * purecap so there is no wide data capability to derive from. We verified
 * (docs/37) that no single segment-wide .data cap is obtainable: &global
 * caps are tightly bounded and the edata/end linker markers are
 * zero-length.
 *
 * Solution (the data analogue of the call trampoline): at CODEGEN time —
 * where `&global` IS a valid (tight) capability in C++ — register each
 * needed global's cap in this table and hand the generated code a 1-based
 * id. Thread::_cap_data_base is seeded with a capability to the table's
 * backing array; generated code does
 *     ldr  dst, [rthread, #cap_data_base_offset]   ; dst = &table[0]
 *     ldr  dst, [dst, #(id-1)*16]                   ; dst = table[id-1] = &global
 * and then accesses the global through `dst` (a valid, correctly-bounded
 * cap straight from C++). Scales to any number of globals.
 *
 * The table is pre-sized so its backing array never reallocates (which
 * would move the base seeded into the threads). All interpreter globals
 * are registered during interpreter generation at VM init, well under the
 * capacity.
 */

#include "precompiled.hpp"
#include "memory/allocation.hpp"
#include "utilities/growableArray.hpp"

// Generous fixed capacity: interpreter+stub globals number in the tens;
// 1024 ensures the backing array is allocated once and never moved.
static const int CAP_DATA_TABLE_CAPACITY = 1024;
static GrowableArray<address>* _cap_data_table = nullptr;

// Register `global` (a valid C++ capability to a libjvm.so global) and
// return its 1-based id, deduplicating by exact capability address.
int cap_data_id_for(address global) {
  if (_cap_data_table == nullptr) {
    _cap_data_table = new (ResourceObj::C_HEAP, mtInternal)
        GrowableArray<address>(CAP_DATA_TABLE_CAPACITY, mtInternal);
  }
  for (int i = 0; i < _cap_data_table->length(); i++) {
    if (_cap_data_table->at(i) == global) return i + 1;
  }
  guarantee(_cap_data_table->length() < CAP_DATA_TABLE_CAPACITY,
            "cap_data_table overflow — raise CAP_DATA_TABLE_CAPACITY");
  _cap_data_table->append(global);
  return _cap_data_table->length();
}

// Capability to the table's backing array (element 0), with bounds
// covering the whole pre-sized array. Seeded into Thread::_cap_data_base.
// Returns nullptr if nothing has been registered yet.
address cap_data_table_base() {
  if (_cap_data_table == nullptr || _cap_data_table->length() == 0) {
    return nullptr;
  }
  return (address)_cap_data_table->adr_at(0);
}
