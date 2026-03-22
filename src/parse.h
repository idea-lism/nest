#pragma once

#include <stdbool.h>
#include <stdint.h>

// Token IDs for the .nest syntax (used as action_id in automata).
// Derived from specs/bootstrap.nest.
//
// IMPORTANT: Within each DFA scope, tokens that should win over '.' (TOK_CHAR)
// must have a LOWER numeric ID than TOK_CHAR (due to MIN-RULE).
enum {
  TOK_COMMENT = 1,
  TOK_SPACE,
  TOK_NL,

  // section headers
  TOK_SECTION_VPA,
  TOK_SECTION_PEG,

  // VPA scope tokens
  TOK_HOOK_BEGIN,
  TOK_HOOK_END,
  TOK_HOOK_FAIL,
  TOK_HOOK_UNPARSE,
  TOK_VPA_ID,
  TOK_MACRO_ID,
  TOK_USER_HOOK_ID,
  TOK_TOK_ID,
  TOK_STATE_ID,
  TOK_SCOPE_BEGIN,
  TOK_SCOPE_END,

  // directive keywords
  TOK_KW_STATE,
  TOK_KW_IGNORE,
  TOK_KW_EFFECT,
  TOK_KW_KEYWORD,

  // ops
  TOK_OPS_EQ,
  TOK_OPS_PIPE,

  // scope-transition tokens (consumed by pushdown, not emitted to stream)
  // These must come before content tokens to win by MIN-RULE
  TOK_RE_BEGIN,
  TOK_RE_END,
  TOK_NEG_CLASS_BEGIN,
  TOK_CLASS_BEGIN,
  TOK_CLASS_END,
  TOK_STR_BEGIN,
  TOK_STR_END,

  // regexp content tokens
  TOK_RE_DOT,
  TOK_RE_SPACE_CLASS,
  TOK_RE_WORD_CLASS,
  TOK_RE_DIGIT_CLASS,
  TOK_RE_HEX_CLASS,
  TOK_RE_BOF,
  TOK_RE_EOF,
  TOK_RE_ALT,
  TOK_RE_LPAREN,
  TOK_RE_RPAREN,
  TOK_RE_MAYBE,
  TOK_RE_PLUS,
  TOK_RE_STAR,
  TOK_RANGE_START,

  // chars (shared across re, charclass, string scopes)
  // Must be LAST among tokens used in re/charclass/string scopes
  TOK_CODEPOINT,
  TOK_C_ESCAPE,
  TOK_PLAIN_ESCAPE,
  TOK_CHAR,

  // PEG scope tokens
  TOK_PEG_ID,
  TOK_PEG_TOK_ID,
  TOK_TAG_ID,
  TOK_PEG_ASSIGN,
  TOK_BRANCHES_BEGIN,
  TOK_BRANCHES_END,
  TOK_PEG_LT,
  TOK_PEG_GT,
  TOK_PEG_QUESTION,
  TOK_PEG_PLUS,
  TOK_PEG_STAR,

  TOK_COUNT
};

// Scope IDs
enum {
  SCOPE_MAIN = 0,
  SCOPE_VPA,
  SCOPE_PEG,
  SCOPE_RE,
  SCOPE_CHARCLASS,
  SCOPE_CHARS,
  SCOPE_DQUOTE_STR,
  SCOPE_SQUOTE_STR,
  SCOPE_IGNORES,
  SCOPE_COUNT
};

#include "re_ast.h"
