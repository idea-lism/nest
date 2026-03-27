// PEG IR helpers: thin abstraction over LLVM IR for packrat parser generation.
// Each opcode maps to a small sequence of LLVM instructions via irwriter.

#include "peg_ir.h"

#include <stdio.h>
#include <string.h>

void peg_ir_tok(IrWriter* w, char* out, int32_t out_size, const char* token_id, const char* col) {
  char args[128];
  snprintf(args, sizeof(args), "i32 %s, i32 %s", token_id, col);
  irwriter_call_ret(w, out, out_size, "i32", "match_tok", args);
}

void peg_ir_call(IrWriter* w, char* out, int32_t out_size, const char* rule_name, const char* table,
                 const char* col) {
  char func_name[128];
  snprintf(func_name, sizeof(func_name), "parse_%s", rule_name);

  char col_ext[32];
  irwriter_sext(w, col_ext, sizeof(col_ext), "i32", col, "i64");

  char args[256];
  snprintf(args, sizeof(args), "ptr %s, i64 %s", table, col_ext);
  irwriter_call_ret(w, out, out_size, "i32", func_name, args);
}

void peg_ir_memo_get(IrWriter* w, char* out, int32_t out_size, const char* col_type, const char* table,
                     const char* col, int32_t field_idx, int32_t slot_idx) {
  char gep_buf[32], indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 %d, i32 %d", col, field_idx, slot_idx);
  irwriter_gep(w, gep_buf, sizeof(gep_buf), col_type, table, indices);
  irwriter_load(w, out, out_size, "i32", gep_buf);
}

void peg_ir_memo_set(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t field_idx,
                     int32_t slot_idx, const char* val) {
  char gep_buf[32], indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 %d, i32 %d", col, field_idx, slot_idx);
  irwriter_gep(w, gep_buf, sizeof(gep_buf), col_type, table, indices);
  irwriter_store(w, "i32", val, gep_buf);
}

void peg_ir_fail_if_neg(IrWriter* w, const char* val, const char* fail_bb, const char* cont_bb) {
  char cmp[32];
  irwriter_icmp_imm(w, cmp, sizeof(cmp), "slt", "i32", val, 0);
  irwriter_br_cond(w, cmp, fail_bb, cont_bb);
}

void peg_ir_is_neg(IrWriter* w, char* out, int32_t out_size, const char* val) {
  irwriter_icmp_imm(w, out, out_size, "slt", "i32", val, 0);
}

void peg_ir_select(IrWriter* w, char* out, int32_t out_size, const char* cond, const char* ty, const char* a,
                   const char* b) {
  irwriter_select(w, out, out_size, cond, ty, a, b);
}

void peg_ir_add(IrWriter* w, char* out, int32_t out_size, const char* a, const char* b) {
  irwriter_binop(w, out, out_size, "add", "i32", a, b);
}

void peg_ir_phi2(IrWriter* w, char* out, int32_t out_size, const char* ty, const char* v1, const char* bb1,
                 const char* v2, const char* bb2) {
  irwriter_phi2(w, out, out_size, ty, v1, bb1, v2, bb2);
}

void peg_ir_save(IrWriter* w, const char* stack, const char* col) {
  char args[128];
  snprintf(args, sizeof(args), "ptr %s, i32 %s", stack, col);
  irwriter_call_void_fmt(w, "bt_push", args);
}

void peg_ir_restore(IrWriter* w, char* out, int32_t out_size, const char* stack) {
  char args[64];
  snprintf(args, sizeof(args), "ptr %s", stack);
  irwriter_call_ret(w, out, out_size, "i32", "bt_peek", args);
}

void peg_ir_discard(IrWriter* w, const char* stack) {
  char args[64];
  snprintf(args, sizeof(args), "ptr %s", stack);
  irwriter_call_void_fmt(w, "bt_pop", args);
}

void peg_ir_bit_test(IrWriter* w, char* out, int32_t out_size, const char* col_type, const char* table,
                     const char* col, int32_t seg_idx, int32_t rule_bit) {
  char gep_buf[32], indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 0, i32 %d", col, seg_idx);
  irwriter_gep(w, gep_buf, sizeof(gep_buf), col_type, table, indices);

  char bits[32];
  irwriter_load(w, bits, sizeof(bits), "i32", gep_buf);

  char masked[32];
  irwriter_binop_imm(w, masked, sizeof(masked), "and", "i32", bits, rule_bit);
  irwriter_icmp_imm(w, out, out_size, "ne", "i32", masked, 0);
}

void peg_ir_bit_deny(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                     int32_t rule_bit) {
  char gep_buf[32], indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 0, i32 %d", col, seg_idx);
  irwriter_gep(w, gep_buf, sizeof(gep_buf), col_type, table, indices);

  char bits[32];
  irwriter_load(w, bits, sizeof(bits), "i32", gep_buf);

  char cleared[32];
  irwriter_binop_imm(w, cleared, sizeof(cleared), "and", "i32", bits, ~rule_bit);
  irwriter_store(w, "i32", cleared, gep_buf);
}

void peg_ir_bit_exclude(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                        int32_t rule_bit) {
  char gep_buf[32], indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 0, i32 %d", col, seg_idx);
  irwriter_gep(w, gep_buf, sizeof(gep_buf), col_type, table, indices);

  char bits[32];
  irwriter_load(w, bits, sizeof(bits), "i32", gep_buf);

  char kept[32];
  irwriter_binop_imm(w, kept, sizeof(kept), "and", "i32", bits, rule_bit);
  irwriter_store(w, "i32", kept, gep_buf);
}

void peg_ir_declare_externs(IrWriter* w, int32_t has_backtrack) {
  irwriter_declare(w, "i32", "match_tok", "i32, i32");
  if (has_backtrack) {
    irwriter_declare(w, "void", "bt_push", "ptr, i32");
    irwriter_declare(w, "i32", "bt_peek", "ptr");
    irwriter_declare(w, "void", "bt_pop", "ptr");
  }
}
