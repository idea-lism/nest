// specs/peg_ir.md
#pragma once

#include "irwriter.h"
#include "peg.h"
#include "symtab.h"

#include <stdint.h>

// --- Code gen: gen(pattern, col, on_fail) dispatcher ---
// Generates IR for an entire rule body: inits BtStack, calls gen(), packs branch_id if needed.
// Returns packed match result (branch_id<<16 | len, or just len).
IrVal peg_ir_gen_rule_body(IrWriter* w, PegUnit* seq, Symtab* tokens, const char* col_type, int32_t has_branches,
                           int32_t fail_label);

// --- Memoize table ops ---
IrVal peg_ir_memo_get(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t field_idx,
                      int32_t slot_idx);
void peg_ir_memo_set(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t field_idx,
                     int32_t slot_idx, IrVal val);

// --- Bit ops (row_shared mode) ---
IrVal peg_ir_bit_test(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                      int32_t rule_bit);
void peg_ir_bit_deny(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                     int32_t rule_bit);
void peg_ir_bit_exclude(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                        int32_t rule_bit);

// --- Module-level declarations ---
void peg_ir_declare_externs(IrWriter* w);
void peg_ir_emit_bt_defs(IrWriter* w);
