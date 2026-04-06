// specs/peg_ir.md

#include "peg_ir.h"
#include "darray.h"
#include "symtab.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// --- Internal helpers ---

static void _fmt_reg(char* out, int32_t out_size, IrVal reg) { snprintf(out, (size_t)out_size, "%%r%d", (int)reg); }

static IrVal _gep_raw(IrWriter* w, const char* base_ty, IrVal ptr, const char* indices_fmt, ...)
    __attribute__((format(printf, 4, 5)));
static IrVal _gep_raw(IrWriter* w, const char* base_ty, IrVal ptr, const char* indices_fmt, ...) {
  IrVal r = irwriter_next_reg(w);
  irwriter_rawf(w, "  %%r%d = getelementptr %s, ptr ", (int)r, base_ty);
  irwriter_emit_val(w, ptr);
  irwriter_raw(w, ", ");
  va_list ap;
  va_start(ap, indices_fmt);
  irwriter_vrawf(w, indices_fmt, ap);
  va_end(ap);
  irwriter_raw(w, "\n");
  return r;
}

// --- Token ID resolution (gen-internal) ---

static int32_t _token_id(Symtab* tokens, const char* name) { return symtab_intern(tokens, name); }

// --- Low-level ops (internal to this file) ---

static IrVal _tok(IrWriter* w, int32_t token_id, const char* col) {
  return irwriter_call_retf(w, "i32", "match_tok", "i32 %d, i32 %s", token_id, col);
}

static IrVal _call(IrWriter* w, const char* rule_name, const char* table, const char* col) {
  IrVal col_ext = irwriter_sext(w, "i32", irwriter_imm(w, col), "i64");
  IrVal r = irwriter_next_reg(w);
  irwriter_rawf(w, "  %%r%d = call i32 @parse_%s(ptr %s, i64 %%r%d)\n", (int)r, rule_name, table, (int)col_ext);
  return r;
}

static void _save(IrWriter* w, IrVal stack, const char* col) {
  irwriter_call_void_fmtf(w, "save", "ptr %%r%d, i32 %s", (int)stack, col);
}

static IrVal _restore(IrWriter* w, IrVal stack) {
  return irwriter_call_retf(w, "i32", "restore", "ptr %%r%d", (int)stack);
}

static void _discard(IrWriter* w, IrVal stack) { irwriter_call_void_fmtf(w, "discard", "ptr %%r%d", (int)stack); }

// --- gen() context ---

typedef struct {
  IrWriter* w;
  Symtab* tokens;
  const char* col_type;
  IrVal bt_stack;
  IrVal slot_val_ptr;
  int32_t* branch_ids;
  int32_t branch_offset;
} GenCtx;

// --- Leaf call ---

static IrVal _emit_leaf_call(GenCtx* ctx, PegUnit* unit, const char* col_expr) {
  if (unit->kind == PEG_ID) {
    return _call(ctx->w, unit->name, "%table", col_expr);
  }
  if (unit->kind == PEG_TOK) {
    int32_t tok_id = _token_id(ctx->tokens, unit->name);
    return _tok(ctx->w, tok_id, col_expr);
  }
  return irwriter_imm(ctx->w, "-1");
}

static IrVal _to_val(GenCtx* ctx, const char* val) { return irwriter_imm(ctx->w, val); }

// --- gen() sub-generators ---

static IrVal _gen_ir(GenCtx* ctx, PegUnit* unit, const char* col_expr, int32_t fail_label);

static IrVal _gen_empty(GenCtx* ctx) { return irwriter_imm(ctx->w, "0"); }

static IrVal _gen_leaf(GenCtx* ctx, PegUnit* unit, const char* col_expr, int32_t fail_label) {
  IrVal r = _emit_leaf_call(ctx, unit, col_expr);

  int32_t ok = irwriter_label(ctx->w);
  irwriter_br_cond(ctx->w, irwriter_icmp(ctx->w, "slt", "i32", r, irwriter_imm(ctx->w, "0")), fail_label, ok);
  irwriter_bb_at(ctx->w, ok);

  return r;
}

static IrVal _gen_optional(GenCtx* ctx, PegUnit* unit, const char* col_expr) {
  int32_t try_bb = irwriter_label(ctx->w);
  int32_t miss_bb = irwriter_label(ctx->w);
  int32_t done_bb = irwriter_label(ctx->w);

  IrVal r = _emit_leaf_call(ctx, unit, col_expr);
  IrVal zero = _gen_empty(ctx);

  irwriter_br_cond(ctx->w, irwriter_icmp(ctx->w, "slt", "i32", r, irwriter_imm(ctx->w, "0")), miss_bb, try_bb);

  irwriter_bb_at(ctx->w, try_bb);
  irwriter_br(ctx->w, done_bb);

  irwriter_bb_at(ctx->w, miss_bb);
  irwriter_br(ctx->w, done_bb);

  irwriter_bb_at(ctx->w, done_bb);
  return irwriter_phi2(ctx->w, "i32", r, try_bb, zero, miss_bb);
}

static IrVal _gen_greedy_loop(GenCtx* ctx, PegUnit* unit, const char* col_expr, IrVal acc_ptr, int32_t loop_bb,
                              int32_t body_bb, int32_t end_bb) {
  irwriter_bb_at(ctx->w, loop_bb);
  IrVal cur_acc = irwriter_load(ctx->w, "i32", acc_ptr);
  IrVal next_col = irwriter_binop(ctx->w, "add", "i32", _to_val(ctx, col_expr), cur_acc);

  char next_col_s[16];
  _fmt_reg(next_col_s, sizeof(next_col_s), next_col);
  IrVal r = _emit_leaf_call(ctx, unit, next_col_s);
  irwriter_br_cond(ctx->w, irwriter_icmp(ctx->w, "slt", "i32", r, irwriter_imm(ctx->w, "0")), end_bb, body_bb);

  irwriter_bb_at(ctx->w, body_bb);
  IrVal prev = irwriter_load(ctx->w, "i32", acc_ptr);
  IrVal next = irwriter_binop(ctx->w, "add", "i32", prev, r);
  irwriter_store(ctx->w, "i32", next, acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  irwriter_bb_at(ctx->w, end_bb);
  return irwriter_load(ctx->w, "i32", acc_ptr);
}

static IrVal _gen_plus(GenCtx* ctx, PegUnit* unit, const char* col_expr, int32_t fail_label) {
  int32_t ok_bb = irwriter_label(ctx->w);
  int32_t loop_bb = irwriter_label(ctx->w);
  int32_t body_bb = irwriter_label(ctx->w);
  int32_t end_bb = irwriter_label(ctx->w);

  IrVal acc_ptr = irwriter_alloca(ctx->w, "i32");
  irwriter_store(ctx->w, "i32", irwriter_imm(ctx->w, "0"), acc_ptr);

  IrVal first = _emit_leaf_call(ctx, unit, col_expr);
  irwriter_br_cond(ctx->w, irwriter_icmp(ctx->w, "slt", "i32", first, irwriter_imm(ctx->w, "0")), fail_label, ok_bb);

  irwriter_bb_at(ctx->w, ok_bb);
  irwriter_store(ctx->w, "i32", first, acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  return _gen_greedy_loop(ctx, unit, col_expr, acc_ptr, loop_bb, body_bb, end_bb);
}

static IrVal _gen_star(GenCtx* ctx, PegUnit* unit, const char* col_expr) {
  int32_t loop_bb = irwriter_label(ctx->w);
  int32_t body_bb = irwriter_label(ctx->w);
  int32_t end_bb = irwriter_label(ctx->w);

  IrVal acc_ptr = irwriter_alloca(ctx->w, "i32");
  irwriter_store(ctx->w, "i32", irwriter_imm(ctx->w, "0"), acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  return _gen_greedy_loop(ctx, unit, col_expr, acc_ptr, loop_bb, body_bb, end_bb);
}

static IrVal _gen_interlace_loop(GenCtx* ctx, PegUnit* unit, PegUnit* sep, const char* col_expr, IrVal acc_ptr,
                                 int32_t loop_bb, int32_t sep_ok_bb, int32_t body_bb, int32_t end_bb) {
  irwriter_bb_at(ctx->w, loop_bb);
  IrVal cur_acc = irwriter_load(ctx->w, "i32", acc_ptr);
  IrVal cur_col = irwriter_binop(ctx->w, "add", "i32", _to_val(ctx, col_expr), cur_acc);

  char cur_col_s[16];
  _fmt_reg(cur_col_s, sizeof(cur_col_s), cur_col);
  IrVal sr = _emit_leaf_call(ctx, sep, cur_col_s);
  irwriter_br_cond(ctx->w, irwriter_icmp(ctx->w, "slt", "i32", sr, irwriter_imm(ctx->w, "0")), end_bb, sep_ok_bb);

  irwriter_bb_at(ctx->w, sep_ok_bb);
  IrVal after_sep = irwriter_binop(ctx->w, "add", "i32", cur_col, sr);
  char after_sep_s[16];
  _fmt_reg(after_sep_s, sizeof(after_sep_s), after_sep);
  IrVal er = _emit_leaf_call(ctx, unit, after_sep_s);
  irwriter_br_cond(ctx->w, irwriter_icmp(ctx->w, "slt", "i32", er, irwriter_imm(ctx->w, "0")), end_bb, body_bb);

  irwriter_bb_at(ctx->w, body_bb);
  IrVal prev_acc = irwriter_load(ctx->w, "i32", acc_ptr);
  IrVal sep_elem = irwriter_binop(ctx->w, "add", "i32", sr, er);
  IrVal next = irwriter_binop(ctx->w, "add", "i32", prev_acc, sep_elem);
  irwriter_store(ctx->w, "i32", next, acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  irwriter_bb_at(ctx->w, end_bb);
  return irwriter_load(ctx->w, "i32", acc_ptr);
}

static IrVal _gen_plus_interlace(GenCtx* ctx, PegUnit* unit, PegUnit* sep, const char* col_expr, int32_t fail_label) {
  int32_t first_ok_bb = irwriter_label(ctx->w);
  int32_t loop_bb = irwriter_label(ctx->w);
  int32_t sep_ok_bb = irwriter_label(ctx->w);
  int32_t body_bb = irwriter_label(ctx->w);
  int32_t end_bb = irwriter_label(ctx->w);

  IrVal acc_ptr = irwriter_alloca(ctx->w, "i32");
  irwriter_store(ctx->w, "i32", irwriter_imm(ctx->w, "0"), acc_ptr);

  IrVal first = _emit_leaf_call(ctx, unit, col_expr);
  irwriter_br_cond(ctx->w, irwriter_icmp(ctx->w, "slt", "i32", first, irwriter_imm(ctx->w, "0")), fail_label,
                   first_ok_bb);

  irwriter_bb_at(ctx->w, first_ok_bb);
  irwriter_store(ctx->w, "i32", first, acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  return _gen_interlace_loop(ctx, unit, sep, col_expr, acc_ptr, loop_bb, sep_ok_bb, body_bb, end_bb);
}

static IrVal _gen_star_interlace(GenCtx* ctx, PegUnit* unit, PegUnit* sep, const char* col_expr) {
  int32_t first_ok_bb = irwriter_label(ctx->w);
  int32_t loop_bb = irwriter_label(ctx->w);
  int32_t sep_ok_bb = irwriter_label(ctx->w);
  int32_t body_bb = irwriter_label(ctx->w);
  int32_t end_bb = irwriter_label(ctx->w);
  int32_t empty_bb = irwriter_label(ctx->w);

  IrVal acc_ptr = irwriter_alloca(ctx->w, "i32");
  irwriter_store(ctx->w, "i32", irwriter_imm(ctx->w, "0"), acc_ptr);

  IrVal first = _emit_leaf_call(ctx, unit, col_expr);
  irwriter_br_cond(ctx->w, irwriter_icmp(ctx->w, "slt", "i32", first, irwriter_imm(ctx->w, "0")), empty_bb,
                   first_ok_bb);

  irwriter_bb_at(ctx->w, first_ok_bb);
  irwriter_store(ctx->w, "i32", first, acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  IrVal result = _gen_interlace_loop(ctx, unit, sep, col_expr, acc_ptr, loop_bb, sep_ok_bb, body_bb, end_bb);

  irwriter_bb_at(ctx->w, empty_bb);
  irwriter_br(ctx->w, end_bb);

  return result;
}

// --- Main gen() dispatcher ---

static IrVal _gen_ir(GenCtx* ctx, PegUnit* unit, const char* col_expr, int32_t fail_label) {
  if (unit->kind == PEG_TOK || unit->kind == PEG_ID) {
    if (unit->multiplier == 0) {
      return _gen_leaf(ctx, unit, col_expr, fail_label);
    }
    if (unit->multiplier == '?') {
      return _gen_optional(ctx, unit, col_expr);
    }
    if (unit->multiplier == '+') {
      if (unit->interlace && unit->ninterlace > 0) {
        return _gen_plus_interlace(ctx, unit, unit->interlace, col_expr, fail_label);
      }
      return _gen_plus(ctx, unit, col_expr, fail_label);
    }
    if (unit->multiplier == '*') {
      if (unit->interlace && unit->ninterlace > 0) {
        return _gen_star_interlace(ctx, unit, unit->interlace, col_expr);
      }
      return _gen_star(ctx, unit, col_expr);
    }
  }

  if (unit->kind == PEG_SEQ) {
    int32_t n = (int32_t)darray_size(unit->children);
    if (n == 0) {
      return _gen_empty(ctx);
    }

    IrVal total_ptr = irwriter_alloca(ctx->w, "i32");
    irwriter_store(ctx->w, "i32", irwriter_imm(ctx->w, "0"), total_ptr);

    int32_t running_branch_offset = 0;
    for (int32_t i = 0; i < n; i++) {
      IrVal prev = irwriter_load(ctx->w, "i32", total_ptr);
      IrVal child_col = irwriter_binop(ctx->w, "add", "i32", _to_val(ctx, col_expr), prev);

      char child_col_s[16];
      _fmt_reg(child_col_s, sizeof(child_col_s), child_col);

      if (unit->children[i].kind == PEG_BRANCHES) {
        ctx->branch_offset = running_branch_offset;
        running_branch_offset += (int32_t)darray_size(unit->children[i].children);
      }

      IrVal child_len = _gen_ir(ctx, &unit->children[i], child_col_s, fail_label);

      IrVal new_total = irwriter_load(ctx->w, "i32", total_ptr);
      IrVal updated = irwriter_binop(ctx->w, "add", "i32", new_total, child_len);
      irwriter_store(ctx->w, "i32", updated, total_ptr);
    }
    return irwriter_load(ctx->w, "i32", total_ptr);
  }

  if (unit->kind == PEG_BRANCHES) {
    int32_t n = (int32_t)darray_size(unit->children);
    if (n == 0) {
      return _gen_empty(ctx);
    }

    int32_t done_bb = irwriter_label(ctx->w);
    IrVal result_ptr = irwriter_alloca(ctx->w, "i32");

    _save(ctx->w, ctx->bt_stack, col_expr);

    for (int32_t i = 0; i < n; i++) {
      int32_t is_last = (i == n - 1);
      int32_t alt_bb = is_last ? -1 : irwriter_label(ctx->w);
      int32_t ft = is_last ? fail_label : alt_bb;

      IrVal r = _gen_ir(ctx, &unit->children[i], col_expr, ft);

      _discard(ctx->w, ctx->bt_stack);
      irwriter_store(ctx->w, "i32", r, result_ptr);
      if (ctx->slot_val_ptr >= 0 && ctx->branch_ids) {
        int32_t bid_val = ctx->branch_ids[ctx->branch_offset + i];
        if (bid_val >= 1) {
          irwriter_store(ctx->w, "i32", irwriter_imm_int(ctx->w, bid_val), ctx->slot_val_ptr);
        } else {
          int32_t branch_idx = -(bid_val + 1);
          IrVal bi = irwriter_imm_int(ctx->w, branch_idx << 16);
          IrVal packed = irwriter_binop(ctx->w, "or", "i32", bi, r);
          IrVal plus3 = irwriter_binop(ctx->w, "add", "i32", packed, irwriter_imm(ctx->w, "3"));
          IrVal negated = irwriter_binop(ctx->w, "sub", "i32", irwriter_imm(ctx->w, "0"), plus3);
          irwriter_store(ctx->w, "i32", negated, ctx->slot_val_ptr);
        }
      }
      irwriter_br(ctx->w, done_bb);

      if (!is_last) {
        irwriter_bb_at(ctx->w, alt_bb);
        IrVal restored = _restore(ctx->w, ctx->bt_stack);
        _discard(ctx->w, ctx->bt_stack);
        char restored_s[16];
        _fmt_reg(restored_s, sizeof(restored_s), restored);
        _save(ctx->w, ctx->bt_stack, restored_s);
      }
    }

    irwriter_bb_at(ctx->w, done_bb);
    return irwriter_load(ctx->w, "i32", result_ptr);
  }

  return _gen_empty(ctx);
}

// --- Public: rule body code generation ---

IrVal peg_ir_gen_rule_body(IrWriter* w, PegUnit* seq, Symtab* tokens, const char* col_type, int32_t* branch_ids,
                           int32_t n_branch_ids, int32_t fail_label, IrVal slot_val_ptr) {
  (void)n_branch_ids;

  IrVal bt_stack = irwriter_alloca(w, "%BtStack");
  IrVal bt_top_ptr = _gep_raw(w, "%BtStack", bt_stack, "i32 0, i32 1");
  irwriter_store(w, "i32", irwriter_imm(w, "-1"), bt_top_ptr);

  GenCtx ctx = {
      .w = w,
      .tokens = tokens,
      .col_type = col_type,
      .bt_stack = bt_stack,
      .slot_val_ptr = slot_val_ptr,
      .branch_ids = branch_ids,
      .branch_offset = 0,
  };
  IrVal match_len = _gen_ir(&ctx, seq, "%col", fail_label);

  if (!branch_ids && slot_val_ptr >= 0) {
    irwriter_store(w, "i32", match_len, slot_val_ptr);
  }
  return match_len;
}

// --- Public: memoize table ops ---

IrVal peg_ir_memo_get(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t field_idx,
                      int32_t slot_idx) {
  IrVal tbl = irwriter_imm(w, table);
  IrVal g = _gep_raw(w, col_type, tbl, "i32 %s, i32 %d, i32 %d", col, field_idx, slot_idx);
  return irwriter_load(w, "i32", g);
}

void peg_ir_memo_set(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t field_idx,
                     int32_t slot_idx, IrVal val) {
  IrVal tbl = irwriter_imm(w, table);
  IrVal g = _gep_raw(w, col_type, tbl, "i32 %s, i32 %d, i32 %d", col, field_idx, slot_idx);
  irwriter_store(w, "i32", val, g);
}

// --- Public: backtrack stack definitions ---

void peg_ir_emit_bt_defs(IrWriter* w) {
  irwriter_type_def(w, "BtStack", "{ [16 x i32], i32 }");
  irwriter_raw(w, "\n");

  irwriter_raw(w, "define internal void @save(ptr %stack, i32 %col) {\n"
                  "entry:\n"
                  "  %top_ptr = getelementptr %BtStack, ptr %stack, i32 0, i32 1\n"
                  "  %top = load i32, ptr %top_ptr\n"
                  "  %new_top = add i32 %top, 1\n"
                  "  store i32 %new_top, ptr %top_ptr\n"
                  "  %slot = getelementptr %BtStack, ptr %stack, i32 0, i32 0, i32 %new_top\n"
                  "  store i32 %col, ptr %slot\n"
                  "  ret void\n"
                  "}\n\n");

  irwriter_raw(w, "define internal i32 @restore(ptr %stack) {\n"
                  "entry:\n"
                  "  %top_ptr = getelementptr %BtStack, ptr %stack, i32 0, i32 1\n"
                  "  %top = load i32, ptr %top_ptr\n"
                  "  %slot = getelementptr %BtStack, ptr %stack, i32 0, i32 0, i32 %top\n"
                  "  %val = load i32, ptr %slot\n"
                  "  ret i32 %val\n"
                  "}\n\n");

  irwriter_raw(w, "define internal void @discard(ptr %stack) {\n"
                  "entry:\n"
                  "  %top_ptr = getelementptr %BtStack, ptr %stack, i32 0, i32 1\n"
                  "  %top = load i32, ptr %top_ptr\n"
                  "  %new_top = add i32 %top, -1\n"
                  "  store i32 %new_top, ptr %top_ptr\n"
                  "  ret void\n"
                  "}\n\n");
}

// --- Public: bit ops (row_shared mode) ---

IrVal peg_ir_bit_test(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                      int32_t rule_bit) {
  IrVal tbl = irwriter_imm(w, table);
  IrVal g = _gep_raw(w, col_type, tbl, "i32 %s, i32 0, i32 %d", col, seg_idx);
  IrVal bits = irwriter_load(w, "i32", g);
  IrVal rb = irwriter_imm_int(w, rule_bit);
  IrVal m = irwriter_binop(w, "and", "i32", bits, rb);
  return irwriter_icmp(w, "ne", "i32", m, irwriter_imm(w, "0"));
}

void peg_ir_bit_deny(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                     int32_t rule_bit) {
  IrVal tbl = irwriter_imm(w, table);
  IrVal g = _gep_raw(w, col_type, tbl, "i32 %s, i32 0, i32 %d", col, seg_idx);
  IrVal bits = irwriter_load(w, "i32", g);
  IrVal mask = irwriter_imm_int(w, ~rule_bit);
  IrVal cleared = irwriter_binop(w, "and", "i32", bits, mask);
  irwriter_store(w, "i32", cleared, g);
}

void peg_ir_bit_exclude(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                        int32_t rule_bit) {
  IrVal tbl = irwriter_imm(w, table);
  IrVal g = _gep_raw(w, col_type, tbl, "i32 %s, i32 0, i32 %d", col, seg_idx);
  IrVal bits = irwriter_load(w, "i32", g);
  IrVal rb = irwriter_imm_int(w, rule_bit);
  IrVal kept = irwriter_binop(w, "and", "i32", bits, rb);
  irwriter_store(w, "i32", kept, g);
}

// --- Public: extern declarations ---

void peg_ir_declare_externs(IrWriter* w) { irwriter_declare(w, "i32", "match_tok", "i32, i32"); }
