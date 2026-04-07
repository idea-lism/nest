#pragma once

#include "header_writer.h"
#include "irwriter.h"
#include "peg.h"
#include "re_ir.h"
#include "symtab.h"

#include <stdbool.h>
#include <stdint.h>

// VPA unit: one element in a VPA rule body
typedef enum {
  VPA_REGEXP,
  VPA_REF,
  VPA_SCOPE,
} VpaUnitKind;

typedef struct VpaUnit VpaUnit;

struct VpaUnit {
  VpaUnitKind kind;
  ReIr re;           // a flattened regexp representation
  bool binary_mode;  // true if tagged with 'b' mode
  char* name;        // tok_id name (without @) or ref name (owned)
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
  bool has_parser; // set by pp_match_scopes when a matching PEG rule exists
} VpaRule;

// Keyword entry
typedef struct {
  char* group; // (owned)
  int32_t lit_off;
  int32_t lit_len;
  const char* src; // source pointer for accessing literal text
} KeywordEntry;

// Effect declaration
typedef struct {
  char* hook_name;  // (owned)
  int32_t* effects; // darray
} EffectDecl;

// Ignore entry
typedef struct {
  Symtab names;
} IgnoreSet;

// Input to vpa_gen
typedef struct {
  VpaRule* rules;         // darray
  KeywordEntry* keywords; // darray
  EffectDecl* effects;    // darray
  const char* src;
} VpaGenInput;

void vpa_gen(VpaGenInput* input, HeaderWriter* hw, IrWriter* w, const char* main_func_name);
