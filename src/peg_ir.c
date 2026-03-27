// PEG IR helpers: thin abstraction over LLVM IR for packrat parser generation.

#include "peg_ir.h"

#include <stdio.h>
#include <string.h>

int32_t peg_ir_tok(IrWriter* w, const char* token_id, const char* col) {
  char args[128];
  snprintf(args, sizeof(args), "i32 %s, i32 %s", token_id, col);
  return irwriter_call_ret(w, "i32", "match_tok", args);
}

int32_t peg_ir_call(IrWriter* w, const char* rule_name, const char* table, const char* col) {
  char func_name[128];
  snprintf(func_name, sizeof(func_name), "parse_%s", rule_name);

  char col_ext[16];
  snprintf(col_ext, sizeof(col_ext), "%%r%d", irwriter_sext(w, "i32", col, "i64"));

  char args[256];
  snprintf(args, sizeof(args), "ptr %s, i64 %s", table, col_ext);
  return irwriter_call_ret(w, "i32", func_name, args);
}

int32_t peg_ir_memo_get(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t field_idx,
                        int32_t slot_idx) {
  int32_t tbl = irwriter_param(w, "ptr", table);
  char indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 %d, i32 %d", col, field_idx, slot_idx);
  int32_t g = irwriter_gep(w, col_type, tbl, indices);
  return irwriter_load(w, "i32", g);
}

void peg_ir_memo_set(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t field_idx,
                     int32_t slot_idx, int32_t val_reg) {
  int32_t tbl = irwriter_param(w, "ptr", table);
  char indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 %d, i32 %d", col, field_idx, slot_idx);
  int32_t g = irwriter_gep(w, col_type, tbl, indices);
  irwriter_store(w, "i32", val_reg, g);
}

// Backtrack stack: { [16 x i32], i32 } = { data, top }
// top starts at -1 (empty). push increments then stores, peek loads at top, pop decrements.

void peg_ir_emit_bt_defs(IrWriter* w) {
  irwriter_type_def(w, "BtStack", "{ [16 x i32], i32 }");
  irwriter_raw(w, "\n");

  irwriter_raw(w, "define internal void @backtrack_push(ptr %stack, i32 %col) {\n"
                  "entry:\n"
                  "  %top_ptr = getelementptr %BtStack, ptr %stack, i32 0, i32 1\n"
                  "  %top = load i32, ptr %top_ptr\n"
                  "  %new_top = add i32 %top, 1\n"
                  "  store i32 %new_top, ptr %top_ptr\n"
                  "  %slot = getelementptr %BtStack, ptr %stack, i32 0, i32 0, i32 %new_top\n"
                  "  store i32 %col, ptr %slot\n"
                  "  ret void\n"
                  "}\n\n");

  irwriter_raw(w, "define internal i32 @backtrack_restore(ptr %stack) {\n"
                  "entry:\n"
                  "  %top_ptr = getelementptr %BtStack, ptr %stack, i32 0, i32 1\n"
                  "  %top = load i32, ptr %top_ptr\n"
                  "  %slot = getelementptr %BtStack, ptr %stack, i32 0, i32 0, i32 %top\n"
                  "  %val = load i32, ptr %slot\n"
                  "  ret i32 %val\n"
                  "}\n\n");

  irwriter_raw(w, "define internal void @backtrack_pop(ptr %stack) {\n"
                  "entry:\n"
                  "  %top_ptr = getelementptr %BtStack, ptr %stack, i32 0, i32 1\n"
                  "  %top = load i32, ptr %top_ptr\n"
                  "  %new_top = add i32 %top, -1\n"
                  "  store i32 %new_top, ptr %top_ptr\n"
                  "  ret void\n"
                  "}\n\n");
}

void peg_ir_backtrack_push(IrWriter* w, int32_t stack, const char* col) {
  char args[128];
  snprintf(args, sizeof(args), "ptr %%r%d, i32 %s", stack, col);
  irwriter_call_void_fmt(w, "backtrack_push", args);
}

int32_t peg_ir_backtrack_restore(IrWriter* w, int32_t stack) {
  char args[64];
  snprintf(args, sizeof(args), "ptr %%r%d", stack);
  return irwriter_call_ret(w, "i32", "backtrack_restore", args);
}

void peg_ir_backtrack_pop(IrWriter* w, int32_t stack) {
  char args[64];
  snprintf(args, sizeof(args), "ptr %%r%d", stack);
  irwriter_call_void_fmt(w, "backtrack_pop", args);
}

int32_t peg_ir_bit_test(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                        int32_t rule_bit) {
  int32_t tbl = irwriter_param(w, "ptr", table);
  char indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 0, i32 %d", col, seg_idx);
  int32_t g = irwriter_gep(w, col_type, tbl, indices);
  int32_t bits = irwriter_load(w, "i32", g);
  int32_t rb = irwriter_imm(w, "i32", rule_bit);
  int32_t m = irwriter_binop(w, "and", "i32", bits, rb);
  return irwriter_icmp_imm(w, "ne", "i32", m, 0);
}

void peg_ir_bit_deny(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                     int32_t rule_bit) {
  int32_t tbl = irwriter_param(w, "ptr", table);
  char indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 0, i32 %d", col, seg_idx);
  int32_t g = irwriter_gep(w, col_type, tbl, indices);
  int32_t bits = irwriter_load(w, "i32", g);
  int32_t mask = irwriter_imm(w, "i32", ~rule_bit);
  int32_t cleared = irwriter_binop(w, "and", "i32", bits, mask);
  irwriter_store(w, "i32", cleared, g);
}

void peg_ir_bit_exclude(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                        int32_t rule_bit) {
  int32_t tbl = irwriter_param(w, "ptr", table);
  char indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 0, i32 %d", col, seg_idx);
  int32_t g = irwriter_gep(w, col_type, tbl, indices);
  int32_t bits = irwriter_load(w, "i32", g);
  int32_t rb = irwriter_imm(w, "i32", rule_bit);
  int32_t kept = irwriter_binop(w, "and", "i32", bits, rb);
  irwriter_store(w, "i32", kept, g);
}

void peg_ir_declare_externs(IrWriter* w) { irwriter_declare(w, "i32", "match_tok", "i32, i32"); }
