// specs/peg_ir.md
#pragma once

#include "irwriter.h"
#include "peg.h"

#include <stdint.h>

// --- Code gen: gen(pattern, col, on_fail) dispatcher ---
// Generates IR for an entire rule body from a ScopedRule graph.
// For branch rules, stores the slot indicator via slot_val_ptr.
// For non-branch rules, stores match_len there.
// Always returns match_len.
IrVal peg_ir_gen_rule_body(IrWriter* w, ScopedRule* rules, int32_t root_id, const char* col_type, int32_t* branch_ids,
                           int32_t n_branch_ids, int32_t fail_label, IrVal slot_val_ptr);

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
