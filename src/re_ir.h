#pragma once

#include "re.h"

#include <stdint.h>

typedef enum {
  RE_IR_RANGE_BEGIN,      // current_range = re_range_new()
  RE_IR_RANGE_END,        // re_append_range(current_range), current_range = NULL
  RE_IR_RANGE_NEG,        // re_range_neg(current_range)
  RE_IR_RANGE_IC,         // (before range_end) re_range_ic(current_range)
  RE_IR_APPEND_CH,        // current_range ? re_range_add(ch) : re_append_ch(ch)
  RE_IR_APPEND_CH_IC,     // current_range ? re_range_add(ch) : re_append_ch_ic(ch)
  RE_IR_APPEND_GROUP_S,   // \s group
  RE_IR_APPEND_GROUP_W,   // \w group
  RE_IR_APPEND_GROUP_D,   // \d group
  RE_IR_APPEND_GROUP_H,   // \h group
  RE_IR_APPEND_GROUP_DOT, // . group
  RE_IR_APPEND_C_ESCAPE,  // start = escape symbol char (b/f/n/r/t/v)
  RE_IR_APPEND_HEX,       // start/end = packed hex codepoint
  RE_IR_LPAREN,           // re_lparen()
  RE_IR_RPAREN,           // re_rparen()
  RE_IR_FORK,             // re_fork() on new branches
  RE_IR_LOOP_BACK,        // aut_epsilon(cur_state, group_start) for + and * loops
  RE_IR_ACTION,           // re_action()
  RE_IR_FRAG_REF,         // fragment reference (cp_start, cp_size) - resolved after %define
} ReIrKind;

typedef struct {
  ReIrKind kind;
  int32_t start;
  int32_t end;
} ReIrOp;

typedef ReIrOp* ReIr; // darray

void re_ir_free(ReIr ir);

ReIr re_ir_clone(ReIr src);

ReIr re_ir_new(void);

ReIr re_ir_emit(ReIr ir, ReIrKind kind, int32_t start, int32_t end);

ReIr re_ir_emit_ch(ReIr ir, int32_t cp);

ReIr re_ir_build_literal(const char* src, int32_t cp_off, int32_t cp_len);

void re_ir_exec(Re* re, ReIr ir, DebugInfo di);
