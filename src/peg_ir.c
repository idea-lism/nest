// specs/peg_ir.md — PEG IR helpers
//
// Col ownership: only peg_ir_seq advances ctx->col_index after each child.
// All other helpers (term, call, choice, maybe, plus, star) read col_index
// but do NOT permanently advance it. They return consumed length.
// On failure they branch to ctx->fail_label; col_index may be dirty and
// the caller is responsible for save/restore via the stack.

#include "peg_ir.h"
#include "darray.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================
// Shared LLVM helper definitions (emitted once per module)
// ============================================================

void peg_ir_emit_helpers(IrWriter* w) {
  irwriter_rawf(w, "\ndefine internal ptr @table_gep(ptr %%table, i32 %%col, i32 %%col_sizeof, i32 %%off) {\n");
  irwriter_rawf(w, "entry:\n");
  irwriter_rawf(w, "  %%a = sext i32 %%col to i64\n");
  irwriter_rawf(w, "  %%b = sext i32 %%col_sizeof to i64\n");
  irwriter_rawf(w, "  %%c = mul i64 %%a, %%b\n");
  irwriter_rawf(w, "  %%d = sext i32 %%off to i64\n");
  irwriter_rawf(w, "  %%e = add i64 %%c, %%d\n");
  irwriter_rawf(w, "  %%p = getelementptr i8, ptr %%table, i64 %%e\n");
  irwriter_rawf(w, "  ret ptr %%p\n");
  irwriter_rawf(w, "}\n");

  irwriter_rawf(w, "\ndefine internal void @save(ptr %%sp_a, ptr %%col_a) {\n");
  irwriter_rawf(w, "entry:\n");
  irwriter_rawf(w, "  %%sp = load ptr, ptr %%sp_a\n");
  irwriter_rawf(w, "  %%nsp = getelementptr i64, ptr %%sp, i64 1\n");
  irwriter_rawf(w, "  store ptr %%nsp, ptr %%sp_a\n");
  irwriter_rawf(w, "  %%col = load i32, ptr %%col_a\n");
  irwriter_rawf(w, "  %%c64 = sext i32 %%col to i64\n");
  irwriter_rawf(w, "  store i64 %%c64, ptr %%nsp\n");
  irwriter_rawf(w, "  ret void\n");
  irwriter_rawf(w, "}\n");

  irwriter_rawf(w, "\ndefine internal void @restore(ptr %%sp_a, ptr %%col_a) {\n");
  irwriter_rawf(w, "entry:\n");
  irwriter_rawf(w, "  %%sp = load ptr, ptr %%sp_a\n");
  irwriter_rawf(w, "  %%v = load i64, ptr %%sp\n");
  irwriter_rawf(w, "  %%col = trunc i64 %%v to i32\n");
  irwriter_rawf(w, "  store i32 %%col, ptr %%col_a\n");
  irwriter_rawf(w, "  ret void\n");
  irwriter_rawf(w, "}\n");

  irwriter_rawf(w, "\ndefine internal i1 @bit_test(ptr %%tbl, i32 %%col, i32 %%csz, i32 %%off, i32 %%bit) {\n");
  irwriter_rawf(w, "entry:\n");
  irwriter_rawf(w, "  %%p = call ptr @table_gep(ptr %%tbl, i32 %%col, i32 %%csz, i32 %%off)\n");
  irwriter_rawf(w, "  %%v = load i32, ptr %%p\n");
  irwriter_rawf(w, "  %%a = and i32 %%v, %%bit\n");
  irwriter_rawf(w, "  %%r = icmp ne i32 %%a, 0\n");
  irwriter_rawf(w, "  ret i1 %%r\n");
  irwriter_rawf(w, "}\n");

  irwriter_rawf(w, "\ndefine internal void @bit_deny(ptr %%tbl, i32 %%col, i32 %%csz, i32 %%off, i32 %%bit) {\n");
  irwriter_rawf(w, "entry:\n");
  irwriter_rawf(w, "  %%p = call ptr @table_gep(ptr %%tbl, i32 %%col, i32 %%csz, i32 %%off)\n");
  irwriter_rawf(w, "  %%v = load i32, ptr %%p\n");
  irwriter_rawf(w, "  %%m = xor i32 %%bit, -1\n");
  irwriter_rawf(w, "  %%c = and i32 %%v, %%m\n");
  irwriter_rawf(w, "  store i32 %%c, ptr %%p\n");
  irwriter_rawf(w, "  ret void\n");
  irwriter_rawf(w, "}\n");

  irwriter_rawf(w, "\ndefine internal void @bit_exclude(ptr %%tbl, i32 %%col, i32 %%csz, i32 %%off, i32 %%bit) {\n");
  irwriter_rawf(w, "entry:\n");
  irwriter_rawf(w, "  %%p = call ptr @table_gep(ptr %%tbl, i32 %%col, i32 %%csz, i32 %%off)\n");
  irwriter_rawf(w, "  %%v = load i32, ptr %%p\n");
  irwriter_rawf(w, "  %%k = and i32 %%v, %%bit\n");
  irwriter_rawf(w, "  store i32 %%k, ptr %%p\n");
  irwriter_rawf(w, "  ret void\n");
  irwriter_rawf(w, "}\n");
}

// ============================================================
// Table GEP: table_base + col * col_sizeof + byte_offset
// ============================================================

static IrVal _table_gep(PegIrCtx* ctx, IrVal col, int32_t byte_offset) {
  IrWriter* w = ctx->w;
  IrVal r = irwriter_next_reg(w);
  irwriter_rawf(w, "  %%r%d = call ptr @table_gep(ptr ", (int)r);
  irwriter_emit_val(w, ctx->table);
  irwriter_rawf(w, ", i32 ");
  irwriter_emit_val(w, col);
  irwriter_rawf(w, ", i32 %d, i32 %d)\n", ctx->col_sizeof, byte_offset);
  return r;
}

// ============================================================
// Slot read / write
// ============================================================

IrVal peg_ir_read_slot(PegIrCtx* ctx, IrVal col, uint32_t slot_index) {
  IrVal ptr = _table_gep(ctx, col, ctx->slots_offset + (int32_t)slot_index * 4);
  return irwriter_load(ctx->w, "i32", ptr);
}

void peg_ir_write_slot(PegIrCtx* ctx, IrVal col, uint32_t slot_index, IrVal val) {
  IrVal ptr = _table_gep(ctx, col, ctx->slots_offset + (int32_t)slot_index * 4);
  irwriter_store(ctx->w, "i32", val, ptr);
}

// ============================================================
// Bit operations (row_shared) — emit calls to @bit_test/deny/exclude
// ============================================================

IrVal peg_ir_bit_test(PegIrCtx* ctx, IrVal col, uint32_t seg_idx, uint32_t rule_bit) {
  IrWriter* w = ctx->w;
  int32_t byte_off = ctx->bits_offset + (int32_t)seg_idx * 4;
  IrVal r = irwriter_next_reg(w);
  irwriter_rawf(w, "  %%r%d = call i1 @bit_test(ptr ", (int)r);
  irwriter_emit_val(w, ctx->table);
  irwriter_rawf(w, ", i32 ");
  irwriter_emit_val(w, col);
  irwriter_rawf(w, ", i32 %d, i32 %d, i32 %u)\n", ctx->col_sizeof, byte_off, rule_bit);
  return r;
}

void peg_ir_bit_deny(PegIrCtx* ctx, IrVal col, uint32_t seg_idx, uint32_t rule_bit) {
  IrWriter* w = ctx->w;
  int32_t byte_off = ctx->bits_offset + (int32_t)seg_idx * 4;
  irwriter_rawf(w, "  call void @bit_deny(ptr ");
  irwriter_emit_val(w, ctx->table);
  irwriter_rawf(w, ", i32 ");
  irwriter_emit_val(w, col);
  irwriter_rawf(w, ", i32 %d, i32 %d, i32 %u)\n", ctx->col_sizeof, byte_off, rule_bit);
}

void peg_ir_bit_exclude(PegIrCtx* ctx, IrVal col, uint32_t seg_idx, uint32_t rule_bit) {
  IrWriter* w = ctx->w;
  int32_t byte_off = ctx->bits_offset + (int32_t)seg_idx * 4;
  irwriter_rawf(w, "  call void @bit_exclude(ptr ");
  irwriter_emit_val(w, ctx->table);
  irwriter_rawf(w, ", i32 ");
  irwriter_emit_val(w, col);
  irwriter_rawf(w, ", i32 %d, i32 %d, i32 %u)\n", ctx->col_sizeof, byte_off, rule_bit);
}

// ============================================================
// Stack operations (i64 slots)
// ============================================================

static IrVal _load_sp(PegIrCtx* ctx) { return irwriter_load(ctx->w, "ptr", ctx->stack); }

static void _store_sp(PegIrCtx* ctx, IrVal sp) { irwriter_store(ctx->w, "ptr", sp, ctx->stack); }

static IrVal _sp_inc(PegIrCtx* ctx) {
  IrWriter* w = ctx->w;
  IrVal sp = _load_sp(ctx);
  IrVal r = irwriter_next_reg(w);
  irwriter_rawf(w, "  %%r%d = getelementptr i64, ptr ", (int)r);
  irwriter_emit_val(w, sp);
  irwriter_rawf(w, ", i64 1\n");
  _store_sp(ctx, r);
  return r;
}

static void _sp_dec(PegIrCtx* ctx) {
  IrWriter* w = ctx->w;
  IrVal sp = _load_sp(ctx);
  IrVal r = irwriter_next_reg(w);
  irwriter_rawf(w, "  %%r%d = getelementptr i64, ptr ", (int)r);
  irwriter_emit_val(w, sp);
  irwriter_rawf(w, ", i64 -1\n");
  _store_sp(ctx, r);
}

static void _stack_save(PegIrCtx* ctx) {
  IrWriter* w = ctx->w;
  irwriter_rawf(w, "  call void @save(ptr ");
  irwriter_emit_val(w, ctx->stack);
  irwriter_rawf(w, ", ptr ");
  irwriter_emit_val(w, ctx->col_index);
  irwriter_rawf(w, ")\n");
}

static void _stack_restore(PegIrCtx* ctx) {
  IrWriter* w = ctx->w;
  irwriter_rawf(w, "  call void @restore(ptr ");
  irwriter_emit_val(w, ctx->stack);
  irwriter_rawf(w, ", ptr ");
  irwriter_emit_val(w, ctx->col_index);
  irwriter_rawf(w, ")\n");
}

static void _stack_discard(PegIrCtx* ctx) { _sp_dec(ctx); }

// ============================================================
// Col helpers
// ============================================================

static IrVal _load_col(PegIrCtx* ctx) { return irwriter_load(ctx->w, "i32", ctx->col_index); }

static void _store_col(PegIrCtx* ctx, IrVal v) { irwriter_store(ctx->w, "i32", v, ctx->col_index); }

static void _advance_col(PegIrCtx* ctx, IrVal n) {
  IrVal c = _load_col(ctx);
  _store_col(ctx, irwriter_binop(ctx->w, "add", "i32", c, n));
}

// ============================================================
// Token access: tokens[col].term_id (Token = 16 bytes, offset 0)
// ============================================================

static IrVal _read_term_id(PegIrCtx* ctx, IrVal col) {
  IrWriter* w = ctx->w;
  IrVal col_ext = irwriter_sext(w, "i32", col, "i64");
  IrVal byte_off = irwriter_binop(w, "mul", "i64", col_ext, irwriter_imm(w, "16"));
  IrVal ptr = irwriter_next_reg(w);
  irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr ", (int)ptr);
  irwriter_emit_val(w, ctx->tokens);
  irwriter_rawf(w, ", i64 ");
  irwriter_emit_val(w, byte_off);
  irwriter_rawf(w, "\n");
  return irwriter_load(w, "i32", ptr);
}

// ============================================================
// Forward decl
// ============================================================

static IrVal _gen_scoped(PegIrCtx* ctx, int32_t scoped_rule_id);
static IrVal _gen_bare(PegIrCtx* ctx, int32_t scoped_rule_id);

// ============================================================
// Terminal — does NOT advance col
// ============================================================

IrVal peg_ir_term(PegIrCtx* ctx, int32_t term_id) {
  IrWriter* w = ctx->w;
  IrVal col = _load_col(ctx);
  IrVal ok = irwriter_icmp(w, "slt", "i32", col, ctx->n_tokens);
  int32_t chk = irwriter_label(w);
  irwriter_br_cond(w, ok, chk, ctx->fail_label);
  irwriter_bb_at(w, chk);

  IrVal tid = _read_term_id(ctx, col);
  IrVal match = irwriter_icmp(w, "eq", "i32", tid, irwriter_imm_int(w, term_id));
  int32_t succ = irwriter_label(w);
  irwriter_br_cond(w, match, succ, ctx->fail_label);
  irwriter_bb_at(w, succ);

  return irwriter_imm_int(w, 1);
}

// ============================================================
// Sub-rule call — does NOT advance col (callee does via its own seq)
// ============================================================

IrVal peg_ir_call(PegIrCtx* ctx, int32_t scoped_rule_id) {
  IrWriter* w = ctx->w;
  ScopedRule* rule = &ctx->rules[scoped_rule_id];

  IrVal new_sp = _sp_inc(ctx);
  int32_t ret_lbl = irwriter_label(w);
  IrVal addr = irwriter_next_reg(w);
  irwriter_rawf(w, "  %%r%d = blockaddress(@parse_%s, %%L%d)\n", (int)addr, ctx->scope_name, ret_lbl);
  irwriter_store(w, "ptr", addr, new_sp);

  IrVal bp = _load_sp(ctx);
  irwriter_store(w, "ptr", bp, ctx->stack_bp);

  irwriter_rawf(w, "  br label %%%s\n", rule->name);

  irwriter_bb_at(w, ret_lbl);
  IrVal ret = irwriter_load(w, "i32", ctx->ret_val);
  IrVal ok = irwriter_icmp(w, "sge", "i32", ret, irwriter_imm_int(w, 0));
  int32_t cont_bb = irwriter_label(w);
  irwriter_br_cond(w, ok, cont_bb, ctx->fail_label);
  irwriter_bb_at(w, cont_bb);
  return ret;
}

// ============================================================
// Sequence — the ONLY helper that advances col_index
//
// Spec: gen(a b, col, fail):
//   r1 = gen(a, col, fail)
//   r2 = gen(b, col + r1, fail)
//   ret = r1 + r2
// ============================================================

IrVal peg_ir_seq(PegIrCtx* ctx, int32_t* seq) {
  IrWriter* w = ctx->w;
  int32_t n = (int32_t)darray_size(seq);
  IrVal acc = irwriter_alloca(w, "i32");
  irwriter_store(w, "i32", irwriter_imm_int(w, 0), acc);

  for (int32_t i = 0; i < n; i++) {
    IrVal r = _gen_scoped(ctx, seq[i]);
    _advance_col(ctx, r);
    IrVal prev = irwriter_load(w, "i32", acc);
    irwriter_store(w, "i32", irwriter_binop(w, "add", "i32", prev, r), acc);
  }
  return irwriter_load(w, "i32", acc);
}

// ============================================================
// Ordered choice — does NOT advance col
// ============================================================

IrVal peg_ir_choice(PegIrCtx* ctx, int32_t* branches) {
  IrWriter* w = ctx->w;
  int32_t n = (int32_t)darray_size(branches);
  if (n == 0) {
    return irwriter_imm_int(w, 0);
  }
  if (n == 1) {
    return _gen_scoped(ctx, branches[0]);
  }

  IrVal outer_fail = ctx->fail_label;
  int32_t done_bb = irwriter_label(w);
  IrVal res = irwriter_alloca(w, "i32");

  _stack_save(ctx);

  for (int32_t i = 0; i < n; i++) {
    bool last = (i == n - 1);
    int32_t alt = last ? outer_fail : irwriter_label(w);
    ctx->fail_label = alt;

    IrVal r = _gen_scoped(ctx, branches[i]);

    _stack_discard(ctx);
    irwriter_store(w, "i32", r, res);
    irwriter_br(w, done_bb);

    if (!last) {
      irwriter_bb_at(w, alt);
      _stack_restore(ctx);
      if (i < n - 2) {
        _stack_save(ctx);
      }
    }
  }

  ctx->fail_label = outer_fail;
  irwriter_bb_at(w, done_bb);
  return irwriter_load(w, "i32", res);
}

// ============================================================
// Optional (?) — does NOT advance col. Always succeeds.
// ============================================================

IrVal peg_ir_maybe(PegIrCtx* ctx, ScopedRuleKind kind, int32_t id) {
  (void)kind;
  IrWriter* w = ctx->w;
  IrVal outer_fail = ctx->fail_label;

  int32_t miss_bb = irwriter_label(w);
  int32_t done_bb = irwriter_label(w);
  IrVal res = irwriter_alloca(w, "i32");
  IrVal col_before = _load_col(ctx);

  ctx->fail_label = miss_bb;
  IrVal r = _gen_bare(ctx, id);
  irwriter_store(w, "i32", r, res);
  irwriter_br(w, done_bb);

  irwriter_bb_at(w, miss_bb);
  _store_col(ctx, col_before);
  irwriter_store(w, "i32", irwriter_imm_int(w, 0), res);
  irwriter_br(w, done_bb);

  irwriter_bb_at(w, done_bb);
  ctx->fail_label = outer_fail;
  return irwriter_load(w, "i32", res);
}

// ============================================================
// One-or-more (+) — does NOT permanently advance col.
// Returns total consumed. Caller advances col.
//
// Spec: gen(e+, col, fail):
//   first = gen(e, col, fail)
//   loop: acc = phi(first, next)
//     r = gen(e, col+acc, end)
//     next = acc + r
//     br loop
//   end: ret = acc
//
// We use col_index as a scratch that we restore on exit.
// ============================================================

IrVal peg_ir_plus(PegIrCtx* ctx, ScopedRuleKind kind, int32_t id, ScopedRuleKind rhs_kind, int32_t rhs_id) {
  (void)kind;
  IrWriter* w = ctx->w;
  bool interlace = (rhs_kind != 0);
  IrVal acc = irwriter_alloca(w, "i32");
  IrVal col_save = irwriter_alloca(w, "i32");
  IrVal col_origin = _load_col(ctx);

  // save original col so we can restore on exit
  irwriter_store(w, "i32", col_origin, col_save);

  // first must succeed
  IrVal first = _gen_bare(ctx, id);
  _advance_col(ctx, first);
  irwriter_store(w, "i32", first, acc);

  int32_t loop_bb = irwriter_label(w);
  int32_t end_bb = irwriter_label(w);
  irwriter_br(w, loop_bb);
  irwriter_bb_at(w, loop_bb);

  IrVal outer_fail = ctx->fail_label;
  ctx->fail_label = end_bb;

  // save col before loop body (for rollback on failure)
  IrVal col_before_iter = _load_col(ctx);
  irwriter_store(w, "i32", col_before_iter, col_save);

  IrVal iter;
  if (interlace) {
    IrVal sr = _gen_bare(ctx, rhs_id);
    _advance_col(ctx, sr);
    IrVal er = _gen_bare(ctx, id);
    _advance_col(ctx, er);
    iter = irwriter_binop(w, "add", "i32", sr, er);
  } else {
    iter = _gen_bare(ctx, id);
    _advance_col(ctx, iter);
  }

  IrVal prev = irwriter_load(w, "i32", acc);
  irwriter_store(w, "i32", irwriter_binop(w, "add", "i32", prev, iter), acc);
  irwriter_br(w, loop_bb);

  irwriter_bb_at(w, end_bb);
  // restore col to before failed iteration
  _store_col(ctx, irwriter_load(w, "i32", col_save));
  // then restore to origin — caller will advance by returned acc
  _store_col(ctx, col_origin);
  ctx->fail_label = outer_fail;

  return irwriter_load(w, "i32", acc);
}

// ============================================================
// Zero-or-more (*) — does NOT permanently advance col.
// Returns total consumed. Always succeeds.
// ============================================================

IrVal peg_ir_star(PegIrCtx* ctx, ScopedRuleKind kind, int32_t id, ScopedRuleKind rhs_kind, int32_t rhs_id) {
  (void)kind;
  IrWriter* w = ctx->w;
  bool interlace = (rhs_kind != 0);
  IrVal acc = irwriter_alloca(w, "i32");
  IrVal col_save = irwriter_alloca(w, "i32");
  IrVal col_origin = _load_col(ctx);
  irwriter_store(w, "i32", irwriter_imm_int(w, 0), acc);

  if (interlace) {
    // e*<sep> = (e (sep e)*)?
    int32_t first_bb = irwriter_label(w);
    int32_t empty_bb = irwriter_label(w);
    int32_t loop_bb = irwriter_label(w);
    int32_t loop_exit_bb = irwriter_label(w);
    int32_t final_bb = irwriter_label(w);

    IrVal outer_fail = ctx->fail_label;

    irwriter_br(w, first_bb);
    irwriter_bb_at(w, first_bb);
    ctx->fail_label = empty_bb;

    IrVal first = _gen_bare(ctx, id);
    _advance_col(ctx, first);
    irwriter_store(w, "i32", first, acc);
    irwriter_br(w, loop_bb);

    irwriter_bb_at(w, loop_bb);
    ctx->fail_label = loop_exit_bb;
    irwriter_store(w, "i32", _load_col(ctx), col_save);

    IrVal sr = _gen_bare(ctx, rhs_id);
    _advance_col(ctx, sr);
    IrVal er = _gen_bare(ctx, id);
    _advance_col(ctx, er);
    IrVal iter = irwriter_binop(w, "add", "i32", sr, er);
    IrVal prev = irwriter_load(w, "i32", acc);
    irwriter_store(w, "i32", irwriter_binop(w, "add", "i32", prev, iter), acc);
    irwriter_br(w, loop_bb);

    irwriter_bb_at(w, loop_exit_bb);
    _store_col(ctx, irwriter_load(w, "i32", col_save));
    irwriter_br(w, final_bb);

    irwriter_bb_at(w, empty_bb);
    irwriter_br(w, final_bb);

    irwriter_bb_at(w, final_bb);
    // restore col to origin; caller advances by returned acc
    _store_col(ctx, col_origin);
    ctx->fail_label = outer_fail;
  } else {
    int32_t loop_bb = irwriter_label(w);
    int32_t end_bb = irwriter_label(w);
    irwriter_br(w, loop_bb);
    irwriter_bb_at(w, loop_bb);

    IrVal outer_fail = ctx->fail_label;
    ctx->fail_label = end_bb;
    irwriter_store(w, "i32", _load_col(ctx), col_save);

    IrVal r = _gen_bare(ctx, id);
    _advance_col(ctx, r);
    IrVal prev = irwriter_load(w, "i32", acc);
    irwriter_store(w, "i32", irwriter_binop(w, "add", "i32", prev, r), acc);
    irwriter_br(w, loop_bb);

    irwriter_bb_at(w, end_bb);
    _store_col(ctx, irwriter_load(w, "i32", col_save));
    // restore col to origin; caller advances by returned acc
    _store_col(ctx, col_origin);
    ctx->fail_label = outer_fail;
  }

  return irwriter_load(w, "i32", acc);
}

// ============================================================
// _gen_bare: match by kind only, ignoring multiplier
// ============================================================

static IrVal _gen_bare(PegIrCtx* ctx, int32_t scoped_rule_id) {
  ScopedRule* r = &ctx->rules[scoped_rule_id];
  switch (r->kind) {
  case SCOPED_RULE_KIND_TERM:
    return peg_ir_term(ctx, r->as.term);
  case SCOPED_RULE_KIND_CALL:
    return peg_ir_call(ctx, r->as.call);
  case SCOPED_RULE_KIND_JOIN:
    return irwriter_imm_int(ctx->w, 0);
  default:
    return peg_ir_call(ctx, scoped_rule_id);
  }
}

// ============================================================
// Element dispatch
// ============================================================

IrVal peg_ir_element(PegIrCtx* ctx, ScopedRuleKind kind, int32_t id) {
  (void)kind;
  return _gen_scoped(ctx, id);
}

// ============================================================
// _gen_scoped: dispatch on ScopedRule kind/multiplier
// ============================================================

static IrVal _gen_scoped(PegIrCtx* ctx, int32_t scoped_rule_id) {
  ScopedRule* r = &ctx->rules[scoped_rule_id];

  if (r->multiplier == '?') {
    return peg_ir_maybe(ctx, r->kind, scoped_rule_id);
  }
  if (r->multiplier == '+') {
    if (r->kind == SCOPED_RULE_KIND_JOIN) {
      return peg_ir_plus(ctx, ctx->rules[r->as.join.lhs].kind, r->as.join.lhs, ctx->rules[r->as.join.rhs].kind,
                         r->as.join.rhs);
    }
    return peg_ir_plus(ctx, r->kind, scoped_rule_id, 0, 0);
  }
  if (r->multiplier == '*') {
    if (r->kind == SCOPED_RULE_KIND_JOIN) {
      return peg_ir_star(ctx, ctx->rules[r->as.join.lhs].kind, r->as.join.lhs, ctx->rules[r->as.join.rhs].kind,
                         r->as.join.rhs);
    }
    return peg_ir_star(ctx, r->kind, scoped_rule_id, 0, 0);
  }

  switch (r->kind) {
  case SCOPED_RULE_KIND_TERM:
    return peg_ir_term(ctx, r->as.term);
  case SCOPED_RULE_KIND_JOIN:
    return irwriter_imm_int(ctx->w, 0);
  case SCOPED_RULE_KIND_CALL:
    return peg_ir_call(ctx, r->as.call);
  default:
    return peg_ir_call(ctx, scoped_rule_id);
  }
}
