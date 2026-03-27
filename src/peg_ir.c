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
  int32_t r = irwriter_sext(w, "i32", col, "i64");
  snprintf(col_ext, sizeof(col_ext), "%%r%d", r);

  char args[256];
  snprintf(args, sizeof(args), "ptr %s, i64 %s", table, col_ext);
  return irwriter_call_ret(w, "i32", func_name, args);
}

int32_t peg_ir_memo_get(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t field_idx,
                        int32_t slot_idx) {
  char indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 %d, i32 %d", col, field_idx, slot_idx);
  char gep[16];
  int32_t g = irwriter_gep(w, col_type, table, indices);
  snprintf(gep, sizeof(gep), "%%r%d", g);
  return irwriter_load(w, "i32", gep);
}

void peg_ir_memo_set(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t field_idx,
                     int32_t slot_idx, const char* val) {
  char indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 %d, i32 %d", col, field_idx, slot_idx);
  char gep[16];
  int32_t g = irwriter_gep(w, col_type, table, indices);
  snprintf(gep, sizeof(gep), "%%r%d", g);
  irwriter_store(w, "i32", val, gep);
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

void peg_ir_backtrack_push(IrWriter* w, const char* stack, const char* col) {
  char args[128];
  snprintf(args, sizeof(args), "ptr %s, i32 %s", stack, col);
  irwriter_call_void_fmt(w, "backtrack_push", args);
}

int32_t peg_ir_backtrack_restore(IrWriter* w, const char* stack) {
  char args[64];
  snprintf(args, sizeof(args), "ptr %s", stack);
  return irwriter_call_ret(w, "i32", "backtrack_restore", args);
}

void peg_ir_backtrack_pop(IrWriter* w, const char* stack) {
  char args[64];
  snprintf(args, sizeof(args), "ptr %s", stack);
  irwriter_call_void_fmt(w, "backtrack_pop", args);
}

int32_t peg_ir_bit_test(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                        int32_t rule_bit) {
  char indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 0, i32 %d", col, seg_idx);
  char gep[16];
  int32_t g = irwriter_gep(w, col_type, table, indices);
  snprintf(gep, sizeof(gep), "%%r%d", g);

  char bits[16];
  int32_t b = irwriter_load(w, "i32", gep);
  snprintf(bits, sizeof(bits), "%%r%d", b);

  char masked[16];
  int32_t m = irwriter_binop_imm(w, "and", "i32", bits, rule_bit);
  snprintf(masked, sizeof(masked), "%%r%d", m);
  return irwriter_icmp_imm(w, "ne", "i32", masked, 0);
}

void peg_ir_bit_deny(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                     int32_t rule_bit) {
  char indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 0, i32 %d", col, seg_idx);
  char gep[16];
  int32_t g = irwriter_gep(w, col_type, table, indices);
  snprintf(gep, sizeof(gep), "%%r%d", g);

  char bits[16];
  int32_t b = irwriter_load(w, "i32", gep);
  snprintf(bits, sizeof(bits), "%%r%d", b);

  char cleared[16];
  int32_t c = irwriter_binop_imm(w, "and", "i32", bits, ~rule_bit);
  snprintf(cleared, sizeof(cleared), "%%r%d", c);
  irwriter_store(w, "i32", cleared, gep);
}

void peg_ir_bit_exclude(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                        int32_t rule_bit) {
  char indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 0, i32 %d", col, seg_idx);
  char gep[16];
  int32_t g = irwriter_gep(w, col_type, table, indices);
  snprintf(gep, sizeof(gep), "%%r%d", g);

  char bits[16];
  int32_t b = irwriter_load(w, "i32", gep);
  snprintf(bits, sizeof(bits), "%%r%d", b);

  char kept[16];
  int32_t k = irwriter_binop_imm(w, "and", "i32", bits, rule_bit);
  snprintf(kept, sizeof(kept), "%%r%d", k);
  irwriter_store(w, "i32", kept, gep);
}

void peg_ir_declare_externs(IrWriter* w) { irwriter_declare(w, "i32", "match_tok", "i32, i32"); }
