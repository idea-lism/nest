#pragma once

#include "header_writer.h"
#include "irwriter.h"
#include "re_ir.h"
#include "symtab.h"

#include <stdbool.h>
#include <stdint.h>

// builtin hook ids in the hooks symtab (start_num=0)
#define HOOK_ID_BEGIN 0
#define HOOK_ID_END 1
#define HOOK_ID_FAIL 2
#define HOOK_ID_UNPARSE 3
#define HOOK_ID_BUILTIN_COUNT 4

// action_unit_id > 0: maps to token_id
// action_unit_id <= 0: maps to -hook_id
typedef int32_t* VpaActionUnits;

typedef enum {
  VPA_RE, // for literals, we have VpaUnit.re built from string, and action_units = { literal_tok_id }
  VPA_CALL,
  VPA_MACRO_REF,
} VpaUnitKind;

typedef struct {
  VpaUnitKind kind;

  // kind = VPA_RE
  ReIr re;          // a flattened regexp representation
  bool binary_mode; // true if tagged with 'b' mode

  // kind = VPA_CALL
  int32_t call_scope_id;

  // kind = VPA_MACRO_REF, expanded in post_process
  char* macro_name;

  // see the `action` rule in bootstrap.nest and numbering in parse.md
  VpaActionUnits action_units;

  int32_t source_line; // line in .nest file (for LLVM IR debug info), 0 = unknown
  int32_t source_col;
} VpaUnit;

// use `VpaUnits` when we mean a darray
typedef VpaUnit* VpaUnits;

typedef struct {
  int32_t scope_id; // scopes[scope_id - tokens_count]
  char* name;       // (owned)
  VpaUnit leader;
  VpaUnits children;
  bool has_parser;
  bool is_macro;       // parse-time: macro rule, removed after inlining
  int32_t source_line; // line in .nest file (for LLVM IR debug info), 0 = unknown
  int32_t source_col;
} VpaScope;

typedef struct {
  int32_t hook_id;
  VpaActionUnits effects;
} EffectDecl;
typedef EffectDecl* EffectDecls;

typedef struct {
  VpaScope* scopes;
  EffectDecls effect_decls; // owned by ParseState
  Symtab tokens;            // owned by ParseState, can be used to lookup token name, start from 1
  Symtab hooks;             // owned by ParseState, can be used to lookup hook name, start from 0
  const char* source_file_name;
  ReFrags re_frags; // owned by ParseState, indexed by frag_id
} VpaGenInput;

void vpa_gen(VpaGenInput* input, HeaderWriter* hw, IrWriter* w, const char* prefix, int32_t main_rule_row);
