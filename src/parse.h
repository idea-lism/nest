#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  SCOPE_MAIN = 0,
  SCOPE_VPA,
  SCOPE_PEG,
  SCOPE_RE,
  SCOPE_RE_STR,
  SCOPE_RE_REF,
  SCOPE_CHARCLASS,
  SCOPE_KEYWORD_STR,
  SCOPE_COUNT
} ScopeId;

// Token IDs for the .nest syntax (used as action_id in automata).
enum {
  TOK_START = SCOPE_COUNT, // first token ID (after scopes)

  // shared tokens
  TOK_END,    // ends any scope
  TOK_IGNORE,
  TOK_NL,

  // scope: vpa
  TOK_UNPARSE_END, // .unparse .end
  TOK_DIRECTIVES_STATE,
  TOK_DIRECTIVES_IGNORE,
  TOK_DIRECTIVES_EFFECT,
  TOK_DIRECTIVES_KEYWORD,
  TOK_HOOK_BEGIN,
  TOK_HOOK_END,
  TOK_HOOK_FAIL,
  TOK_HOOK_UNPARSE,
  TOK_VPA_ID,
  TOK_MACRO_ID,
  TOK_USER_HOOK_ID,
  TOK_TOK_ID,
  TOK_STATE_ID,
  TOK_OPS_EQ,
  TOK_OPS_PIPE,
  TOK_SCOPE_BEGIN,
  TOK_SCOPE_END,

  // shared by re, re_str, charclass, keyword_str
  TOK_CODEPOINT,
  TOK_C_ESCAPE,
  TOK_PLAIN_ESCAPE,
  TOK_CHAR,

  // re scope
  TOK_RE_TAG,
  TOK_RE_DOT,
  TOK_RE_SPACE_CLASS,
  TOK_RE_WORD_CLASS,
  TOK_RE_DIGIT_CLASS,
  TOK_RE_HEX_CLASS,
  TOK_RE_BOF,
  TOK_RE_EOF,
  TOK_RE_OPS_ALT,
  TOK_RE_OPS_LPAREN,
  TOK_RE_OPS_RPAREN,
  TOK_RE_OPS_MAYBE,
  TOK_RE_OPS_PLUS,
  TOK_RE_OPS_STAR,

  // re_ref scope
  TOK_RE_REF,

  // charclass scope
  TOK_CHARCLASS_BEGIN,
  TOK_RANGE_SEP,

  // keyword_str scope
  TOK_KEYWORD_STR,

  // peg scope
  TOK_PEG_ID,
  TOK_PEG_TOK_ID,
  TOK_TAG_ID,
  TOK_BRANCHES_BEGIN,
  TOK_BRANCHES_END,
  TOK_PEG_OPS_LT,
  TOK_PEG_OPS_GT,
  TOK_PEG_OPS_QUESTION,
  TOK_PEG_OPS_PLUS,
  TOK_PEG_OPS_STAR,
  TOK_PEG_OPS_ASSIGN,

  TOK_COUNT
};

#include "re_ast.h"
#include "token_chunk.h"
#include "vpa.h"
#include "peg.h"

// --- Parser state ---

typedef struct {
  const char* src;
  int32_t src_len;
  char* ustr;

  TokenChunk main_chunk;
  int32_t tpos;

  ReAstNode** re_asts; // darray
  void* str_spans;     // darray (StrSpan*)

  VpaRule* vpa_rules;     // darray
  KeywordEntry* keywords; // darray
  IgnoreSet ignores;
  StateDecl* states;   // darray
  EffectDecl* effects; // darray
  PegRule* peg_rules;  // darray

  char error[512];
} ParseState;

// --- Public API ---

bool parse_nest(ParseState* ps, const char* src);

ParseState* parse_state_new(void);
void parse_state_del(ParseState* ps);
const char* parse_get_error(ParseState* ps);

// --- Helper functions ---

void parse_error(ParseState* ps, const char* fmt, ...);
bool parse_has_error(ParseState* ps);
char* parse_sfmt(const char* fmt, ...);
void parse_set_str(char** dst, char* s);
