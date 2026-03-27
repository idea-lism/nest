#pragma once

#include "irwriter.h"

#include <stdint.h>

// tok(token_id, col) — match token at col, returns match length or negative
void peg_ir_tok(IrWriter* w, char* out, int32_t out_size, const char* token_id, const char* col);

// call(rule_name, table, col) — call rule function, returns match length or negative
void peg_ir_call(IrWriter* w, char* out, int32_t out_size, const char* rule_name, const char* table, const char* col);

// memo_get — read memo slot via GEP+load
void peg_ir_memo_get(IrWriter* w, char* out, int32_t out_size, const char* col_type, const char* table,
                     const char* col, int32_t field_idx, int32_t slot_idx);

// memo_set — write memo slot via GEP+store
void peg_ir_memo_set(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t field_idx,
                     int32_t slot_idx, const char* val);

// fail_if_neg(val, fail_bb, cont_bb) — branch to fail_bb if val < 0, else to cont_bb
void peg_ir_fail_if_neg(IrWriter* w, const char* val, const char* fail_bb, const char* cont_bb);

// is_neg(val) — returns i1, true when val < 0
void peg_ir_is_neg(IrWriter* w, char* out, int32_t out_size, const char* val);

// select(cond, ty, a, b)
void peg_ir_select(IrWriter* w, char* out, int32_t out_size, const char* cond, const char* ty, const char* a,
                   const char* b);

// add(a, b) — i32 add
void peg_ir_add(IrWriter* w, char* out, int32_t out_size, const char* a, const char* b);

// phi2(ty, v1, bb1, v2, bb2) — SSA merge
void peg_ir_phi2(IrWriter* w, char* out, int32_t out_size, const char* ty, const char* v1, const char* bb1,
                 const char* v2, const char* bb2);

// Backtrack stack (ordered choice)
void peg_ir_save(IrWriter* w, const char* stack, const char* col);
void peg_ir_restore(IrWriter* w, char* out, int32_t out_size, const char* stack);
void peg_ir_discard(IrWriter* w, const char* stack);

// Row-shared bit operations
void peg_ir_bit_test(IrWriter* w, char* out, int32_t out_size, const char* col_type, const char* table,
                     const char* col, int32_t seg_idx, int32_t rule_bit);
void peg_ir_bit_deny(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                     int32_t rule_bit);
void peg_ir_bit_exclude(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                        int32_t rule_bit);

// Emit extern declarations needed by PEG IR
void peg_ir_declare_externs(IrWriter* w, int32_t has_backtrack);
