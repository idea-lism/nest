#include "re_ir.h"
#include "darray.h"
#include "re.h"
#include "ustr.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

void re_ir_free(ReIr ir) { darray_del(ir); }

ReIr re_ir_clone(ReIr src) {
  if (!src) {
    return NULL;
  }
  int32_t n = (int32_t)darray_size(src);
  ReIr dst = darray_new(sizeof(ReIrOp), (size_t)n);
  memcpy(dst, src, (size_t)n * sizeof(ReIrOp));
  return dst;
}

ReIr re_ir_new(void) { return darray_new(sizeof(ReIrOp), 0); }

ReIr re_ir_emit(ReIr ir, ReIrKind kind, int32_t start, int32_t end, int32_t line, int32_t col) {
  darray_push(ir, ((ReIrOp){.kind = kind, .start = start, .end = end, .line = line, .col = col}));
  return ir;
}

ReIr re_ir_emit_ch(ReIr ir, int32_t cp) { return re_ir_emit(ir, RE_IR_APPEND_CH, cp, cp, 0, 0); }

ReIr re_ir_build_literal(const char* src, int32_t cp_off, int32_t cp_len) {
  ReIr ir = re_ir_new();
  char* s = ustr_slice(src, cp_off, cp_off + cp_len);
  int32_t size = ustr_size(s);
  UstrIter it = {0};
  ustr_iter_init(&it, s, 0);
  for (int32_t i = 0; i < size; i++) {
    int32_t cp = ustr_iter_next(&it);
    if (cp < 0) {
      break;
    }
    ir = re_ir_emit_ch(ir, cp);
  }
  ustr_del(s);
  return ir;
}

// --- re_ir_exec: recursive interpreter with frag inlining ---

typedef struct {
  Re* re;
  const char* source_file_name;
  ReFrags frags;
  int32_t* ref_stack; // darray of frag_ids for recursion detect
  int32_t paren_depth;
  ReIrExecResult result;
} ExecCtx;

static void _exec_ir(ExecCtx* ctx, ReIr ir);

static void _exec_frag_ref(ExecCtx* ctx, ReIrOp* op) {
  int32_t frag_id = op->start;
  int32_t n_frags = ctx->frags ? (int32_t)darray_size(ctx->frags) : 0;
  if (frag_id < 0 || frag_id >= n_frags || !ctx->frags[frag_id]) {
    ctx->result.err_type = RE_IR_ERR_MISSING_FRAG_ID;
    ctx->result.missing_frag_id = frag_id;
    ctx->result.line = op->line;
    ctx->result.col = op->col;
    return;
  }
  // check recursion
  int32_t stack_size = (int32_t)darray_size(ctx->ref_stack);
  for (int32_t i = 0; i < stack_size; i++) {
    if (ctx->ref_stack[i] == frag_id) {
      ctx->result.err_type = RE_IR_ERR_RECURSION;
      ctx->result.line = op->line;
      ctx->result.col = op->col;
      return;
    }
  }
  darray_push(ctx->ref_stack, frag_id);
  _exec_ir(ctx, ctx->frags[frag_id]);
  // pop
  ctx->ref_stack = darray_grow(ctx->ref_stack, darray_size(ctx->ref_stack) - 1);
}

static void _exec_ir(ExecCtx* ctx, ReIr ir) {
  ReRange* range = NULL;
  int32_t n = (int32_t)darray_size(ir);
  for (int32_t i = 0; i < n && ctx->result.err_type == RE_IR_OK; i++) {
    ReIrOp* op = &ir[i];
    DebugInfo di = {op->line, op->col};
    switch (op->kind) {
    case RE_IR_RANGE_BEGIN:
      if (range) {
        ctx->result.err_type = RE_IR_ERR_BRACKET_MISMATCH;
        ctx->result.line = op->line;
        ctx->result.col = op->col;
        if (range) { re_range_del(range); }
        return;
      }
      range = re_range_new();
      break;
    case RE_IR_RANGE_END:
      if (!range) {
        ctx->result.err_type = RE_IR_ERR_BRACKET_MISMATCH;
        ctx->result.line = op->line;
        ctx->result.col = op->col;
        return;
      }
      re_append_range(ctx->re, range, di);
      re_range_del(range);
      range = NULL;
      break;
    case RE_IR_RANGE_NEG:
      if (range) { re_range_neg(range); }
      break;
    case RE_IR_RANGE_IC:
      if (range) { re_range_ic(range); }
      break;
    case RE_IR_APPEND_CH:
      if (range) {
        re_range_add(range, op->start, op->end);
      } else {
        re_append_ch(ctx->re, op->start, di);
      }
      break;
    case RE_IR_APPEND_CH_IC:
      if (range) {
        re_range_add(range, op->start, op->end);
      } else {
        re_append_ch_ic(ctx->re, op->start, di);
      }
      break;
    case RE_IR_APPEND_GROUP_S:
      if (range) { re_append_group_s(ctx->re, range); }
      break;
    case RE_IR_APPEND_GROUP_W:
      if (range) { re_append_group_w(ctx->re, range); }
      break;
    case RE_IR_APPEND_GROUP_D:
      if (range) { re_append_group_d(ctx->re, range); }
      break;
    case RE_IR_APPEND_GROUP_H:
      if (range) { re_append_group_h(ctx->re, range); }
      break;
    case RE_IR_APPEND_GROUP_DOT:
      if (range) { re_append_group_dot(ctx->re, range); }
      break;
    case RE_IR_APPEND_C_ESCAPE:
      re_append_ch(ctx->re, re_c_escape((char)op->start), di);
      break;
    case RE_IR_APPEND_HEX:
      re_append_ch(ctx->re, op->start, di);
      break;
    case RE_IR_LPAREN:
      ctx->paren_depth++;
      re_lparen(ctx->re);
      break;
    case RE_IR_RPAREN:
      if (ctx->paren_depth <= 0) {
        ctx->result.err_type = RE_IR_ERR_PAREN_MISMATCH;
        ctx->result.line = op->line;
        ctx->result.col = op->col;
        return;
      }
      ctx->paren_depth--;
      re_rparen(ctx->re);
      break;
    case RE_IR_FORK:
      re_fork(ctx->re);
      break;
    case RE_IR_LOOP_BACK:
      re_loop_back(ctx->re);
      break;
    case RE_IR_ACTION:
      re_action(ctx->re, op->start);
      break;
    case RE_IR_FRAG_REF:
      _exec_frag_ref(ctx, op);
      break;
    }
  }
  if (range) {
    re_range_del(range);
    if (ctx->result.err_type == RE_IR_OK) {
      ctx->result.err_type = RE_IR_ERR_BRACKET_MISMATCH;
    }
  }
}

ReIrExecResult re_ir_exec(Re* re, ReIr ir, const char* source_file_name, ReFrags frags) {
  ExecCtx ctx = {
      .re = re,
      .source_file_name = source_file_name,
      .frags = frags,
      .ref_stack = darray_new(sizeof(int32_t), 0),
      .result = {.err_type = RE_IR_OK},
  };
  _exec_ir(&ctx, ir);
  if (ctx.result.err_type == RE_IR_OK && ctx.paren_depth > 0) {
    ctx.result.err_type = RE_IR_ERR_PAREN_MISMATCH;
  }
  darray_del(ctx.ref_stack);
  return ctx.result;
}
