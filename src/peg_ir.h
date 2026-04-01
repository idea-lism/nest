#pragma once

#include "irwriter.h"

#include <stdint.h>

IrVal peg_ir_tok(IrWriter* w, const char* token_id, const char* col);
IrVal peg_ir_call(IrWriter* w, const char* rule_name, const char* table, const char* col);

IrVal peg_ir_memo_get(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t field_idx,
                      int32_t slot_idx);
void peg_ir_memo_set(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t field_idx,
                     int32_t slot_idx, IrVal val);

void peg_ir_emit_bt_defs(IrWriter* w);
void peg_ir_backtrack_push(IrWriter* w, IrVal stack, const char* col);
IrVal peg_ir_backtrack_restore(IrWriter* w, IrVal stack);
void peg_ir_backtrack_pop(IrWriter* w, IrVal stack);

IrVal peg_ir_bit_test(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                      int32_t rule_bit);
void peg_ir_bit_deny(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                     int32_t rule_bit);
void peg_ir_bit_exclude(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                        int32_t rule_bit);

void peg_ir_declare_externs(IrWriter* w);
