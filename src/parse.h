#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  SCOPE_START,

  SCOPE_MAIN,
  SCOPE_VPA,
  SCOPE_SCOPE,
  SCOPE_LIT_SCOPE,
  SCOPE_PEG,
  SCOPE_BRANCHES,
  SCOPE_PEG_TAG,
  SCOPE_RE,
  SCOPE_RE_REF,
  SCOPE_CHARCLASS,
  SCOPE_RE_STR,
  SCOPE_PEG_STR,

  SCOPE_COUNT
} ScopeId;

typedef enum {
  ACTION_START = SCOPE_COUNT,

  ACTION_IGNORE,        // .ignore
  ACTION_BEGIN,         // .begin
  ACTION_END,           // .end
  ACTION_UNPARSE,       // .unparse
  ACTION_FAIL,          // .fail
  ACTION_STR_CHECK_END, // .str_check_end

  // composite: since lexer api only accepts single action_id, multiple actions must be combined
  ACTION_UNPARSE_END,       // .unparse .end
  ACTION_SET_RE_MODE_BEGIN, // .set_re_mode .begin
  ACTION_SET_CC_KIND_BEGIN, // .set_cc_kind .begin
  ACTION_SET_QUOTE_BEGIN,   // .set_quote .begin

  ACTION_COUNT
} ActionId;

typedef enum {
  LIT_START = ACTION_COUNT,

  LIT_IGNORE,
  LIT_EFFECT,
  LIT_DEFINE,
  LIT_EQ,
  LIT_OR,
  LIT_INTERLACE_BEGIN,
  LIT_INTERLACE_END,
  LIT_QUESTION,
  LIT_PLUS,
  LIT_STAR,
  LIT_LPAREN,
  LIT_RPAREN,

  LIT_COUNT
} LitId;

typedef enum {
  TOK_START = 1 << 16,

  TOK_NL,

  TOK_TOK_ID,
  TOK_HOOK_BEGIN,
  TOK_HOOK_END,
  TOK_HOOK_FAIL,
  TOK_HOOK_UNPARSE,
  TOK_VPA_ID,
  TOK_MODULE_ID,
  TOK_USER_HOOK_ID,
  TOK_RE_FRAG_ID,

  TOK_CODEPOINT,
  TOK_C_ESCAPE,
  TOK_PLAIN_ESCAPE,
  TOK_CHAR,

  TOK_RE_DOT,
  TOK_RE_SPACE_CLASS,
  TOK_RE_WORD_CLASS,
  TOK_RE_DIGIT_CLASS,
  TOK_RE_HEX_CLASS,
  TOK_RE_BOF,
  TOK_RE_EOF,

  TOK_RE_REF,

  TOK_RANGE_SEP,

  TOK_PEG_ID,
  TOK_PEG_TOK_ID,
  TOK_TAG_ID,

  TOK_COUNT
} TokenId;

#include "peg.h"
#include "token_tree.h"
#include "vpa.h"

typedef struct {
  char* name;
  ReIr re;
} ReFragment;

// Ignore entry (parse-internal, used by post_process for validation)
typedef struct {
  Symtab names;
} IgnoreSet;

struct SharedState;
typedef struct SharedState SharedState;

typedef struct {
  const char* src;
  int32_t src_len;

  TokenTree* tree;

  ReFragment* re_frags;

  VpaScope* vpa_scopes;
  EffectDecls effect_decls;
  Symtab tokens;      // unified token numbering, start from 1
  Symtab hooks;       // hook numbering, start from 0 (.begin=0, .end=1, .fail=2, .unparse=3)
  Symtab scope_names; // scope numbering, start from 0
  Symtab rule_names;  // peg rule numbering, start from 0

  IgnoreSet ignores;
  PegRule* peg_rules;

  SharedState* shared;
  char error[512];
} ParseState;

typedef void* DStr;

bool parse_nest(ParseState* ps, const char* src);

ParseState* parse_state_new(void);
void parse_state_del(ParseState* ps);
const char* parse_get_error(ParseState* ps);

void parse_error(ParseState* ps, const char* fmt, ...);
bool parse_has_error(ParseState* ps);
char* parse_sfmt(const char* fmt, ...);
void parse_set_str(char** dst, char* s);
