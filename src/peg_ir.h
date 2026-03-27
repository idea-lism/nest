#pragma once

#include "irwriter.h"

#include <stdint.h>

// tok(token_id, col) — match token at col, returns match length or negative
int32_t peg_ir_tok(IrWriter* w, const char* token_id, const char* col);

// call(rule_name, table, col) — call rule function, returns match length or negative
int32_t peg_ir_call(IrWriter* w, const char* rule_name, const char* table, const char* col);

// memo_get — read memo slot via GEP+load
int32_t peg_ir_memo_get(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t field_idx,
                        int32_t slot_idx);

// memo_set — write memo slot via GEP+store
void peg_ir_memo_set(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t field_idx,
                     int32_t slot_idx, const char* val);

// Backtrack stack — emits internal IR function definitions (bt_push, bt_peek, bt_pop)
void peg_ir_emit_bt_defs(IrWriter* w);

// save(col) — push col onto backtrack stack
void peg_ir_backtrack_push(IrWriter* w, const char* stack, const char* col);

// backtrack_restore() — peek saved col from backtrack stack
int32_t peg_ir_backtrack_restore(IrWriter* w, const char* stack);

// backtrack_pop() — pop backtrack stack
void peg_ir_backtrack_pop(IrWriter* w, const char* stack);

// Row-shared bit operations
int32_t peg_ir_bit_test(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                        int32_t rule_bit);
void peg_ir_bit_deny(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                     int32_t rule_bit);
void peg_ir_bit_exclude(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                        int32_t rule_bit);

// Emit extern declarations (match_tok)
void peg_ir_declare_externs(IrWriter* w);
