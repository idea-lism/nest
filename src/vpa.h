#pragma once

#include "header_writer.h"
#include "irwriter.h"
#include "peg.h"
#include "re_ir.h"

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
