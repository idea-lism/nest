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
  RE_IR_FRAG_REF,         // fragment reference(start=frag_sym_id) - resolved after %define
} ReIrKind;

typedef struct {
  ReIrKind kind;
  int32_t start;
  int32_t end;

  // debug info
  int32_t line; // 1-based
  int32_t col;  // 1-based, by cp
} ReIrOp;

typedef ReIrOp* ReIr; // darray

typedef ReIr* ReFrags; // darray of ReIr, indexed by frag_id

typedef enum {
  RE_IR_OK,
  RE_IR_ERR_RECURSION,       // frag_ref recurses
  RE_IR_ERR_MISSING_FRAG_ID, // frags size too small or frags[frag_id] is empty
  RE_IR_ERR_PAREN_MISMATCH,  // too many right-paren, or missing right-paren at end
  RE_IR_ERR_BRACKET_MISMATCH,
} ReIrErrKind;

typedef struct {
  ReIrErrKind err_type;
  int missing_frag_id; // set when RE_IR_ERR_MISSING_FRAG_ID
  int line;
  int col;
} ReIrExecResult;

void re_ir_free(ReIr ir);
ReIr re_ir_clone(ReIr src);
ReIr re_ir_new(void);

// emit op with debug location (line=0, col=0 if unknown)
ReIr re_ir_emit(ReIr ir, ReIrKind kind, int32_t start, int32_t end, int32_t line, int32_t col);

// shorthand: emit RE_IR_APPEND_CH with cp as both start and end
ReIr re_ir_emit_ch(ReIr ir, int32_t cp);

// build literal IR from UTF-8 source slice
ReIr re_ir_build_literal(const char* src, int32_t cp_off, int32_t cp_len);

// interpret IR into re.h calls, when met with frag_ref,
// lookup the ReIr in frags and recursively execute it (frags may ref frags too)
// frags can be NULL if no fragments are defined
ReIrExecResult re_ir_exec(Re* re, ReIr ir, const char* source_file_name, ReFrags frags);

