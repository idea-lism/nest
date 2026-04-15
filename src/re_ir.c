#include "re_ir.h"
#include "darray.h"
#include "re.h"
#include "ustr.h"

#include <assert.h>
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

ReIr re_ir_emit(ReIr ir, ReIrKind kind, int32_t start, int32_t end) {
  darray_push(ir, ((ReIrOp){kind, start, end}));
  return ir;
}

ReIr re_ir_emit_ch(ReIr ir, int32_t cp) { return re_ir_emit(ir, RE_IR_APPEND_CH, cp, cp); }

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

void re_ir_exec(Re* re, ReIr ir, DebugInfo di) {
  ReRange* range = NULL;
  int32_t n = (int32_t)darray_size(ir);
  for (int32_t i = 0; i < n; i++) {
    ReIrOp* op = &ir[i];
    switch (op->kind) {
    case RE_IR_RANGE_BEGIN:
      assert(!range);
      range = re_range_new();
      break;
    case RE_IR_RANGE_END:
      assert(range);
      re_append_range(re, range, di);
      re_range_del(range);
      range = NULL;
      break;
    case RE_IR_RANGE_NEG:
      assert(range);
      re_range_neg(range);
      break;
    case RE_IR_RANGE_IC:
      assert(range);
      re_range_ic(range);
      break;
    case RE_IR_APPEND_CH:
      if (range) {
        re_range_add(range, op->start, op->end);
      } else {
        re_append_ch(re, op->start, di);
      }
      break;
    case RE_IR_APPEND_CH_IC:
      if (range) {
        re_range_add(range, op->start, op->end);
      } else {
        re_append_ch_ic(re, op->start, di);
      }
      break;
    case RE_IR_APPEND_GROUP_S:
      assert(range);
      re_append_group_s(re, range);
      break;
    case RE_IR_APPEND_GROUP_W:
      assert(range);
      re_append_group_w(re, range);
      break;
    case RE_IR_APPEND_GROUP_D:
      assert(range);
      re_append_group_d(re, range);
      break;
    case RE_IR_APPEND_GROUP_H:
      assert(range);
      re_append_group_h(re, range);
      break;
    case RE_IR_APPEND_GROUP_DOT:
      assert(range);
      re_append_group_dot(re, range);
      break;
    case RE_IR_APPEND_C_ESCAPE:
      re_append_ch(re, re_c_escape((char)op->start), di);
      break;
    case RE_IR_APPEND_HEX:
      re_append_ch(re, op->start, di);
      break;
    case RE_IR_LPAREN:
      re_lparen(re);
      break;
    case RE_IR_RPAREN:
      re_rparen(re);
      break;
    case RE_IR_FORK:
      re_fork(re);
      break;
    case RE_IR_LOOP_BACK:
      re_loop_back(re);
      break;
    case RE_IR_ACTION:
      re_action(re, op->start);
      break;
    case RE_IR_FRAG_REF:
      break;
    }
  }
  assert(!range);
}
