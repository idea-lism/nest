// specs/peg_ir.md
#include "peg_ir.h"
#include "darray.h"

#include <stdio.h>
#include <stdlib.h>

// --- Stack helpers ---

static void _emit_call_save(PegIrCtx* ctx) {
  IrWriter* w = ctx->ir_writer;
  irwriter_call_void_fmtf(w, "save", "ptr %%r%d, ptr %%r%d", (int)ctx->stack_ptr, (int)ctx->col);
}

static void _emit_call_restore(PegIrCtx* ctx) {
  IrWriter* w = ctx->ir_writer;
  irwriter_call_void_fmtf(w, "restore", "ptr %%r%d, ptr %%r%d", (int)ctx->stack_ptr, (int)ctx->col);
}

// discard: stack-- without restoring col
static void _emit_discard(PegIrCtx* ctx) {
  IrWriter* w = ctx->ir_writer;
  IrVal sp = irwriter_load(w, "ptr", ctx->stack_ptr);
  irwriter_rawf(w, "  %%r%d = getelementptr i64, ptr %%r%d, i64 -1\n", irwriter_next_reg(w), (int)sp);
  IrVal new_sp = (IrVal)(irwriter_next_reg(w) - 1);
  irwriter_store(w, "ptr", new_sp, ctx->stack_ptr);
}

// --- Forward declaration ---

static void _emit_term(PegIrCtx* ctx, ScopedUnit* unit, IrLabel fail_label);
static void _emit_call(PegIrCtx* ctx, ScopedUnit* unit, IrLabel fail_label);
static void _emit_seq(PegIrCtx* ctx, ScopedUnit* unit, IrLabel fail_label);
static void _emit_branches(PegIrCtx* ctx, ScopedUnit* unit, IrLabel fail_label);
static void _emit_maybe(PegIrCtx* ctx, ScopedUnit* unit, IrLabel fail_label);
static void _emit_star(PegIrCtx* ctx, ScopedUnit* unit, IrLabel fail_label);
static void _emit_plus(PegIrCtx* ctx, ScopedUnit* unit, IrLabel fail_label);

// --- Core dispatch ---

void peg_ir_emit_parse(PegIrCtx* ctx, ScopedUnit* unit, IrLabel fail_label) {
  switch (unit->kind) {
  case SCOPED_UNIT_TERM:
    _emit_term(ctx, unit, fail_label);
    break;
  case SCOPED_UNIT_CALL:
    _emit_call(ctx, unit, fail_label);
    break;
  case SCOPED_UNIT_SEQ:
    _emit_seq(ctx, unit, fail_label);
    break;
  case SCOPED_UNIT_BRANCHES:
    _emit_branches(ctx, unit, fail_label);
    break;
  case SCOPED_UNIT_MAYBE:
    _emit_maybe(ctx, unit, fail_label);
    break;
  case SCOPED_UNIT_STAR:
    _emit_star(ctx, unit, fail_label);
    break;
  case SCOPED_UNIT_PLUS:
    _emit_plus(ctx, unit, fail_label);
    break;
  }

  // if unit has tag, set the tag bit
  if (unit->tag_bit_local_offset >= 0) {
    IrWriter* w = ctx->ir_writer;
    IrVal tb = irwriter_load(w, "i64", ctx->tag_bits);
    uint64_t bit = 1ULL << (uint64_t)(unit->tag_bit_local_offset + ctx->tag_bit_offset);
    irwriter_rawf(w, "  %%r%d = or i64 %%r%d, %llu\n", irwriter_next_reg(w), (int)tb, (unsigned long long)bit);
    IrVal new_tb = (IrVal)(irwriter_next_reg(w) - 1);
    irwriter_store(w, "i64", new_tb, ctx->tag_bits);
  }
}

// --- Term: match a single token at current position ---

static void _emit_term(PegIrCtx* ctx, ScopedUnit* unit, IrLabel fail_label) {
  IrWriter* w = ctx->ir_writer;
  IrVal col_val = irwriter_load(w, "i64", ctx->col);

  IrVal n_tok = irwriter_sext(w, "i32", ctx->token_size, "i64");
  IrVal in_bounds = irwriter_icmp(w, "slt", "i64", col_val, n_tok);
  IrLabel bounds_ok = irwriter_label(w);
  irwriter_br_cond(w, in_bounds, bounds_ok, fail_label);
  irwriter_bb_at(w, bounds_ok);

  // tokens[col_val].term_id (field 0)
  irwriter_rawf(w, "  %%r%d = getelementptr %%Token, ptr %%r%d, i64 %%r%d, i32 0\n", irwriter_next_reg(w),
                (int)ctx->tokens, (int)col_val);
  IrVal tid_ptr = (IrVal)(irwriter_next_reg(w) - 1);
  IrVal term_id = irwriter_load(w, "i32", tid_ptr);

  IrVal cmp = irwriter_icmp(w, "eq", "i32", term_id, irwriter_imm_int(w, unit->as.term_id));
  IrLabel ok_bb = irwriter_label(w);
  irwriter_br_cond(w, cmp, ok_bb, fail_label);

  irwriter_bb_at(w, ok_bb);
  IrVal new_col = irwriter_binop(w, "add", "i64", col_val, irwriter_imm_int(w, 1));
  irwriter_store(w, "i64", new_col, ctx->col);
}

// --- Call: pseudo-call to a sub-rule via stack ---

IrLabel peg_ir_emit_call(PegIrCtx* ctx, const char* callee_name) {
  IrWriter* w = ctx->ir_writer;
  IrLabel ret_label = irwriter_label(w);

  darray_push(ctx->ret_labels, ret_label);

  // stack++; stack->ret_site = &&ret_label
  IrVal sp = irwriter_load(w, "ptr", ctx->stack_ptr);
  irwriter_rawf(w, "  %%r%d = getelementptr i64, ptr %%r%d, i64 1\n", irwriter_next_reg(w), (int)sp);
  IrVal sp1 = (IrVal)(irwriter_next_reg(w) - 1);
  irwriter_rawf(w, "  store ptr blockaddress(@%s, %%", ctx->fn_name);
  irwriter_emit_label(w, ret_label);
  irwriter_rawf(w, "), ptr %%r%d, align 8\n", (int)sp1);

  // stack++; stack->col = col
  irwriter_rawf(w, "  %%r%d = getelementptr i64, ptr %%r%d, i64 1\n", irwriter_next_reg(w), (int)sp1);
  IrVal sp2 = (IrVal)(irwriter_next_reg(w) - 1);
  irwriter_store(w, "ptr", sp2, ctx->stack_ptr);
  IrVal col_val = irwriter_load(w, "i64", ctx->col);
  irwriter_store(w, "i64", col_val, sp2);

  // branch to callee
  irwriter_rawf(w, "  br label %%%s\n", callee_name);

  return ret_label;
}

static void _emit_call(PegIrCtx* ctx, ScopedUnit* unit, IrLabel fail_label) {
  IrWriter* w = ctx->ir_writer;
  IrLabel ret_label = peg_ir_emit_call(ctx, unit->as.callee);

  // ret_label: resume after call
  irwriter_bb_at(w, ret_label);
  IrVal parsed = irwriter_load(w, "i64", ctx->parse_result);
  IrVal failed = irwriter_icmp(w, "slt", "i64", parsed, irwriter_imm_int(w, 0));
  IrLabel call_ok_bb = irwriter_label(w);
  irwriter_br_cond(w, failed, fail_label, call_ok_bb);
  irwriter_bb_at(w, call_ok_bb);
}

// --- Sequence ---

static void _emit_seq(PegIrCtx* ctx, ScopedUnit* unit, IrLabel fail_label) {
  IrWriter* w = ctx->ir_writer;
  int32_t n = (int32_t)darray_size(unit->as.children);
  if (n == 0) {
    return;
  }

  _emit_call_save(ctx);

  IrLabel fail_bb = irwriter_label(w);
  IrLabel done_bb = irwriter_label(w);

  for (int32_t i = 0; i < n; i++) {
    peg_ir_emit_parse(ctx, &unit->as.children[i], fail_bb);
  }

  _emit_discard(ctx);
  irwriter_br(w, done_bb);

  irwriter_bb_at(w, fail_bb);
  _emit_call_restore(ctx);
  _emit_discard(ctx);
  irwriter_br(w, fail_label);

  irwriter_bb_at(w, done_bb);
}

// --- Ordered choice (branches) ---

static void _emit_branches(PegIrCtx* ctx, ScopedUnit* unit, IrLabel fail_label) {
  IrWriter* w = ctx->ir_writer;
  int32_t n = (int32_t)darray_size(unit->as.children);
  if (n == 0) {
    return;
  }

  IrLabel done_bb = irwriter_label(w);

  // save(col) once before all alternatives
  _emit_call_save(ctx);

  IrLabel* alt_bbs = malloc((size_t)(n + 1) * sizeof(IrLabel));
  for (int32_t i = 1; i < n; i++) {
    alt_bbs[i] = irwriter_label(w);
  }
  IrLabel last_fail_bb = irwriter_label(w);

  for (int32_t i = 0; i < n; i++) {
    IrLabel this_fail = (i < n - 1) ? alt_bbs[i + 1] : last_fail_bb;

    if (i > 0) {
      irwriter_bb_at(w, alt_bbs[i]);
      _emit_call_restore(ctx);
    }

    peg_ir_emit_parse(ctx, &unit->as.children[i], this_fail);
    _emit_discard(ctx);
    irwriter_br(w, done_bb);
  }

  // all alternatives failed: restore col then discard
  irwriter_bb_at(w, last_fail_bb);
  _emit_call_restore(ctx);
  _emit_discard(ctx);
  irwriter_br(w, fail_label);

  irwriter_bb_at(w, done_bb);

  free(alt_bbs);
}

// --- Maybe (?) ---

static void _emit_maybe(PegIrCtx* ctx, ScopedUnit* unit, IrLabel fail_label) {
  (void)fail_label;
  IrLabel done_bb = irwriter_label(ctx->ir_writer);

  peg_ir_emit_parse(ctx, unit->as.base, done_bb);
  irwriter_br(ctx->ir_writer, done_bb);

  irwriter_bb_at(ctx->ir_writer, done_bb);
}

// --- Star (*) ---

static void _emit_star(PegIrCtx* ctx, ScopedUnit* unit, IrLabel fail_label) {
  (void)fail_label;
  IrWriter* w = ctx->ir_writer;

  bool has_interlace = (unit->as.interlace.rhs != NULL);
  bool body_nullable = unit->as.interlace.lhs->nullable;

  if (!has_interlace) {
    IrLabel loop_bb = irwriter_label(w);
    IrLabel end_bb = irwriter_label(w);

    irwriter_br(w, loop_bb);
    irwriter_bb_at(w, loop_bb);

    IrVal col_before = body_nullable ? irwriter_load(w, "i64", ctx->col) : (IrVal)0;

    peg_ir_emit_parse(ctx, unit->as.interlace.lhs, end_bb);

    // if e is nullable, check advancement to prevent infinite loop
    if (body_nullable) {
      IrVal col_after = irwriter_load(w, "i64", ctx->col);
      IrVal advanced = irwriter_icmp(w, "ne", "i64", col_after, col_before);
      irwriter_br_cond(w, advanced, loop_bb, end_bb);
    } else {
      irwriter_br(w, loop_bb);
    }

    irwriter_bb_at(w, end_bb);
  } else {
    IrLabel loop_bb = irwriter_label(w);
    IrLabel sep_fail_bb = irwriter_label(w);
    IrLabel empty_bb = irwriter_label(w);
    IrLabel end_bb = irwriter_label(w);

    // first element
    peg_ir_emit_parse(ctx, unit->as.interlace.lhs, empty_bb);
    irwriter_br(w, loop_bb);

    // loop
    irwriter_bb_at(w, loop_bb);

    IrVal col_before = body_nullable ? irwriter_load(w, "i64", ctx->col) : (IrVal)0;

    _emit_call_save(ctx);
    peg_ir_emit_parse(ctx, unit->as.interlace.rhs, sep_fail_bb);
    peg_ir_emit_parse(ctx, unit->as.interlace.lhs, sep_fail_bb);
    _emit_discard(ctx);

    // if e is nullable, check advancement to prevent infinite loop
    if (body_nullable) {
      IrVal col_after = irwriter_load(w, "i64", ctx->col);
      IrVal advanced = irwriter_icmp(w, "ne", "i64", col_after, col_before);
      irwriter_br_cond(w, advanced, loop_bb, end_bb);
    } else {
      irwriter_br(w, loop_bb);
    }

    // sep_fail: restore col, discard
    irwriter_bb_at(w, sep_fail_bb);
    _emit_call_restore(ctx);
    _emit_discard(ctx);
    irwriter_br(w, end_bb);

    // empty: 0 matches
    irwriter_bb_at(w, empty_bb);
    irwriter_br(w, end_bb);

    irwriter_bb_at(w, end_bb);
  }
}

// --- Plus (+) ---

static void _emit_plus(PegIrCtx* ctx, ScopedUnit* unit, IrLabel fail_label) {
  IrWriter* w = ctx->ir_writer;

  bool has_interlace = (unit->as.interlace.rhs != NULL);
  bool body_nullable = unit->as.interlace.lhs->nullable;

  // first element must succeed
  peg_ir_emit_parse(ctx, unit->as.interlace.lhs, fail_label);

  if (!has_interlace) {
    IrLabel loop_bb = irwriter_label(w);
    IrLabel end_bb = irwriter_label(w);

    irwriter_br(w, loop_bb);
    irwriter_bb_at(w, loop_bb);

    IrVal col_before = body_nullable ? irwriter_load(w, "i64", ctx->col) : (IrVal)0;

    peg_ir_emit_parse(ctx, unit->as.interlace.lhs, end_bb);

    // if e is nullable, check advancement to prevent infinite loop
    if (body_nullable) {
      IrVal col_after = irwriter_load(w, "i64", ctx->col);
      IrVal advanced = irwriter_icmp(w, "ne", "i64", col_after, col_before);
      irwriter_br_cond(w, advanced, loop_bb, end_bb);
    } else {
      irwriter_br(w, loop_bb);
    }

    irwriter_bb_at(w, end_bb);
  } else {
    IrLabel loop_bb = irwriter_label(w);
    IrLabel sep_fail_bb = irwriter_label(w);
    IrLabel end_bb = irwriter_label(w);

    irwriter_br(w, loop_bb);
    irwriter_bb_at(w, loop_bb);

    IrVal col_before = body_nullable ? irwriter_load(w, "i64", ctx->col) : (IrVal)0;

    _emit_call_save(ctx);
    peg_ir_emit_parse(ctx, unit->as.interlace.rhs, sep_fail_bb);
    peg_ir_emit_parse(ctx, unit->as.interlace.lhs, sep_fail_bb);
    _emit_discard(ctx);

    // if e is nullable, check advancement to prevent infinite loop
    if (body_nullable) {
      IrVal col_after = irwriter_load(w, "i64", ctx->col);
      IrVal advanced = irwriter_icmp(w, "ne", "i64", col_after, col_before);
      irwriter_br_cond(w, advanced, loop_bb, end_bb);
    } else {
      irwriter_br(w, loop_bb);
    }

    irwriter_bb_at(w, sep_fail_bb);
    _emit_call_restore(ctx);
    _emit_discard(ctx);
    irwriter_br(w, end_bb);

    irwriter_bb_at(w, end_bb);
  }
}

// --- Return epilogue ---

void peg_ir_emit_ret(PegIrCtx* ctx) {
  IrWriter* w = ctx->ir_writer;
  // spec: stack-- (pop col pushed by call)
  IrVal sp = irwriter_load(w, "ptr", ctx->stack_ptr);
  irwriter_rawf(w, "  %%r%d = getelementptr i64, ptr %%r%d, i64 -1\n", irwriter_next_reg(w), (int)sp);
  IrVal sp_after_col = (IrVal)(irwriter_next_reg(w) - 1);
  // spec: %ret_addr = stack->ret_site
  IrVal ret_addr = irwriter_load(w, "ptr", sp_after_col);
  // spec: stack--
  irwriter_rawf(w, "  %%r%d = getelementptr i64, ptr %%r%d, i64 -1\n", irwriter_next_reg(w), (int)sp_after_col);
  IrVal sp_after_ret = (IrVal)(irwriter_next_reg(w) - 1);
  // spec: bp = stack
  IrVal bp = sp_after_ret;
  // spec: ret = parsed_tokens
  IrVal result = irwriter_load(w, "i64", ctx->parsed_tokens);
  irwriter_store(w, "i64", result, ctx->parse_result);
  // spec: stack = bp
  irwriter_store(w, "ptr", bp, ctx->stack_ptr);
  // spec: indirectbr ret_addr
  irwriter_rawf(w, "  indirectbr ptr %%r%d, [", (int)ret_addr);
  int32_t n = (int32_t)darray_size(ctx->ret_labels);
  for (int32_t i = 0; i < n; i++) {
    if (i > 0) {
      irwriter_rawf(w, ", ");
    }
    irwriter_rawf(w, "label %%");
    irwriter_emit_label(w, ctx->ret_labels[i]);
  }
  irwriter_rawf(w, "]\n");
}

// --- Helper function definitions ---

void peg_ir_emit_helpers(IrWriter* w) {
  // @save(ptr %stack_ptr_alloca, ptr %col_alloca)
  {
    irwriter_define_startf(w, "save", "internal void @save(ptr %%stack_ptr, ptr %%col)");
    irwriter_bb(w);
    IrVal sp = irwriter_load(w, "ptr", irwriter_imm(w, "%stack_ptr"));
    irwriter_rawf(w, "  %%r%d = getelementptr i64, ptr %%r%d, i64 1\n", irwriter_next_reg(w), (int)sp);
    IrVal sp1 = (IrVal)(irwriter_next_reg(w) - 1);
    irwriter_store(w, "ptr", sp1, irwriter_imm(w, "%stack_ptr"));
    IrVal col_val = irwriter_load(w, "i64", irwriter_imm(w, "%col"));
    irwriter_store(w, "i64", col_val, sp1);
    irwriter_ret_void(w);
    irwriter_define_end(w);
  }

  // @restore(ptr %stack_ptr_alloca, ptr %col_alloca)
  {
    irwriter_define_startf(w, "restore", "internal void @restore(ptr %%stack_ptr, ptr %%col)");
    irwriter_bb(w);
    IrVal sp = irwriter_load(w, "ptr", irwriter_imm(w, "%stack_ptr"));
    IrVal col_val = irwriter_load(w, "i64", sp);
    irwriter_store(w, "i64", col_val, irwriter_imm(w, "%col"));
    irwriter_ret_void(w);
    irwriter_define_end(w);
  }

  // @top(ptr %stack_ptr_alloca) -> i64: get the top stored col (but not update %col)
  {
    irwriter_define_startf(w, "top", "internal i64 @top(ptr %%stack_ptr)");
    irwriter_bb(w);
    IrVal sp = irwriter_load(w, "ptr", irwriter_imm(w, "%stack_ptr"));
    IrVal val = irwriter_load(w, "i64", sp);
    irwriter_ret(w, "i64", val);
    irwriter_define_end(w);
  }
}

void peg_ir_emit_bit_helpers(IrWriter* w) {
  // @bit_test(ptr %table, i64 %col, i64 %col_size, i64 %seg_offset, i64 %rule_bit) -> i1
  {
    irwriter_define_startf(
        w, "bit_test",
        "internal i1 @bit_test(ptr %%table, i64 %%col, i64 %%col_size, i64 %%seg_offset, i64 %%rule_bit)");
    irwriter_bb(w);
    IrVal off = irwriter_binop(w, "mul", "i64", irwriter_imm(w, "%col"), irwriter_imm(w, "%col_size"));
    IrVal off2 = irwriter_binop(w, "add", "i64", off, irwriter_imm(w, "%seg_offset"));
    irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%table, i64 %%r%d\n", irwriter_next_reg(w), (int)off2);
    IrVal ptr = (IrVal)(irwriter_next_reg(w) - 1);
    IrVal seg = irwriter_load(w, "i64", ptr);
    IrVal masked = irwriter_binop(w, "and", "i64", seg, irwriter_imm(w, "%rule_bit"));
    IrVal result = irwriter_icmp(w, "ne", "i64", masked, irwriter_imm_int(w, 0));
    irwriter_ret(w, "i1", result);
    irwriter_define_end(w);
  }

  // @bit_deny(ptr %table, i64 %col, i64 %col_size, i64 %seg_offset, i64 %rule_bit)
  {
    irwriter_define_startf(
        w, "bit_deny",
        "internal void @bit_deny(ptr %%table, i64 %%col, i64 %%col_size, i64 %%seg_offset, i64 %%rule_bit)");
    irwriter_bb(w);
    IrVal off = irwriter_binop(w, "mul", "i64", irwriter_imm(w, "%col"), irwriter_imm(w, "%col_size"));
    IrVal off2 = irwriter_binop(w, "add", "i64", off, irwriter_imm(w, "%seg_offset"));
    irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%table, i64 %%r%d\n", irwriter_next_reg(w), (int)off2);
    IrVal ptr = (IrVal)(irwriter_next_reg(w) - 1);
    IrVal seg = irwriter_load(w, "i64", ptr);
    irwriter_rawf(w, "  %%r%d = xor i64 %%rule_bit, -1\n", irwriter_next_reg(w));
    IrVal neg_reg = (IrVal)(irwriter_next_reg(w) - 1);
    IrVal cleared = irwriter_binop(w, "and", "i64", seg, neg_reg);
    irwriter_store(w, "i64", cleared, ptr);
    irwriter_ret_void(w);
    irwriter_define_end(w);
  }

  // @bit_exclude(ptr %table, i64 %col, i64 %col_size, i64 %seg_offset, i64 %segment_mask, i64 %rule_bit)
  {
    irwriter_define_startf(w, "bit_exclude",
                           "internal void @bit_exclude(ptr %%table, i64 %%col, i64 %%col_size, i64 %%seg_offset, i64 "
                           "%%segment_mask, i64 %%rule_bit)");
    irwriter_bb(w);
    IrVal off = irwriter_binop(w, "mul", "i64", irwriter_imm(w, "%col"), irwriter_imm(w, "%col_size"));
    IrVal off2 = irwriter_binop(w, "add", "i64", off, irwriter_imm(w, "%seg_offset"));
    irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%table, i64 %%r%d\n", irwriter_next_reg(w), (int)off2);
    IrVal ptr = (IrVal)(irwriter_next_reg(w) - 1);
    IrVal old = irwriter_load(w, "i64", ptr);
    // clear segment bits, preserve tag bits
    irwriter_rawf(w, "  %%r%d = xor i64 %%segment_mask, -1\n", irwriter_next_reg(w));
    IrVal neg_seg = (IrVal)(irwriter_next_reg(w) - 1);
    IrVal cleared = irwriter_binop(w, "and", "i64", old, neg_seg);
    IrVal result = irwriter_binop(w, "or", "i64", cleared, irwriter_imm(w, "%rule_bit"));
    irwriter_store(w, "i64", result, ptr);
    irwriter_ret_void(w);
    irwriter_define_end(w);
  }
}

void peg_ir_emit_gep_helpers(IrWriter* w) {
  // @gep_slot(ptr %table, i64 %col, i64 %sizeof_col, i64 %slot_byte_offset) -> ptr
  {
    irwriter_define_startf(w, "gep_slot",
                           "internal ptr @gep_slot(ptr %%table, i64 %%col, i64 %%sizeof_col, i64 %%slot_byte_offset)");
    irwriter_bb(w);
    IrVal row = irwriter_binop(w, "mul", "i64", irwriter_imm(w, "%col"), irwriter_imm(w, "%sizeof_col"));
    IrVal off = irwriter_binop(w, "add", "i64", row, irwriter_imm(w, "%slot_byte_offset"));
    irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%table, i64 %%r%d\n", irwriter_next_reg(w), (int)off);
    irwriter_ret(w, "ptr", (IrVal)(irwriter_next_reg(w) - 1));
    irwriter_define_end(w);
  }

  // @gep_tag(ptr %table, i64 %col, i64 %sizeof_col, i64 %tag_byte_offset) -> ptr
  {
    irwriter_define_startf(w, "gep_tag",
                           "internal ptr @gep_tag(ptr %%table, i64 %%col, i64 %%sizeof_col, i64 %%tag_byte_offset)");
    irwriter_bb(w);
    IrVal row = irwriter_binop(w, "mul", "i64", irwriter_imm(w, "%col"), irwriter_imm(w, "%sizeof_col"));
    IrVal off = irwriter_binop(w, "add", "i64", row, irwriter_imm(w, "%tag_byte_offset"));
    irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%table, i64 %%r%d\n", irwriter_next_reg(w), (int)off);
    irwriter_ret(w, "ptr", (IrVal)(irwriter_next_reg(w) - 1));
    irwriter_define_end(w);
  }

  // @tag_writeback(ptr %table, i64 %col, i64 %sizeof_col, i64 %tag_byte_offset, i64 %clear_mask, i64 %tag_bits)
  {
    irwriter_define_startf(w, "tag_writeback",
                           "internal void @tag_writeback(ptr %%table, i64 %%col, i64 %%sizeof_col, i64 "
                           "%%tag_byte_offset, i64 %%clear_mask, i64 %%tag_bits)");
    irwriter_bb(w);
    IrVal p =
        irwriter_call_retf(w, "ptr", "gep_tag", "ptr %%table, i64 %%col, i64 %%sizeof_col, i64 %%tag_byte_offset");
    IrVal old = irwriter_load(w, "i64", p);
    IrVal cleared = irwriter_binop(w, "and", "i64", old, irwriter_imm(w, "%clear_mask"));
    IrVal combined = irwriter_binop(w, "or", "i64", cleared, irwriter_imm(w, "%tag_bits"));
    irwriter_store(w, "i64", combined, p);
    irwriter_ret_void(w);
    irwriter_define_end(w);
  }
}
