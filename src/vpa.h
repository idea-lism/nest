#pragma once

#include "header_writer.h"
#include "irwriter.h"
#include "peg.h"
#include "re.h"

#include <stdbool.h>
#include <stdint.h>

// VPA unit: one element in a VPA rule body
typedef enum {
  VPA_REGEXP,
  VPA_REF,
  VPA_SCOPE,
  VPA_STATE,
} VpaUnitKind;

typedef struct VpaUnit VpaUnit;

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
  RE_FORK,                // re_fork() on new branches
  RE_IR_ACTION,           // re_action()
} ReIrKind;

typedef struct {
  ReIrKind kind;
  int32_t start;
  int32_t end;
} ReIrOp;

typedef ReIrOp* ReIr; // darray

struct VpaUnit {
  VpaUnitKind kind;
  ReIr re;           // a flattened regexp representation
  bool binary_mode;  // true if tagged with 'b' mode
  char* name;        // tok_id name (without @) or ref name (owned)
  char* state_name;  // VPA_STATE: state matcher name (without $) (owned)
  int32_t hook;      // TOK_HOOK_BEGIN, _END, _FAIL, _UNPARSE, or 0
  char* user_hook;   // (owned, may be NULL)
  VpaUnit* children; // darray
};

// VPA rule
typedef struct {
  char* name;     // (owned)
  VpaUnit* units; // darray
  bool is_scope;
  bool is_macro;
} VpaRule;

// Keyword entry
typedef struct {
  char* group; // (owned)
  int32_t lit_off;
  int32_t lit_len;
  const char* src; // source pointer for accessing literal text
} KeywordEntry;

// State declaration
typedef struct {
  char* name; // (owned)
} StateDecl;

// Effect declaration
typedef struct {
  char* hook_name;  // (owned)
  int32_t* effects; // darray
} EffectDecl;

// Ignore entry
typedef struct {
  char** names; // darray of strdup'd strings
} IgnoreSet;

// Input to vpa_gen
typedef struct {
  VpaRule* rules;         // darray
  KeywordEntry* keywords; // darray
  StateDecl* states;      // darray
  EffectDecl* effects;    // darray
  PegRule* peg_rules;     // darray
  const char* src;
} VpaGenInput;

void vpa_gen(VpaGenInput* input, HeaderWriter* hw, IrWriter* w);
