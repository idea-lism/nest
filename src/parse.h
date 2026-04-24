#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  SCOPE_START = 1, // IMPORTANT: no scope / token at 0, so 0 can be used as sentinel

  SCOPE_MAIN = 1,
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
  ACTION_END,           // .end
  ACTION_UNPARSE,       // .unparse
  ACTION_FAIL,          // .fail
  ACTION_STR_CHECK_END, // .str_check_end
  // .begin hook is scope-specific
  ACTION_BEGIN_PUSH,    // scope has PEG rule mapped (see materializable in parse.md), push token_tree. scope example:
                        // branches
  ACTION_BEGIN_NO_PUSH, // when scope has no PEG rule mapped, no need push. scope example: re_ref

  // composite: since lexer api only accepts single action_id, multiple actions must be combined
  ACTION_UNPARSE_END,       // .unparse .end
  ACTION_SET_RE_MODE_BEGIN, // .set_re_mode .begin
  ACTION_SET_CC_KIND_BEGIN, // .set_cc_kind .begin
  ACTION_SET_QUOTE_BEGIN,   // .set_quote .begin
  ACTION_END_NL,            // .end @nl

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
  LIT_AND,
  LIT_NOT,

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
  TOK_HOOK_NOOP,
  TOK_VPA_ID,
  TOK_MODULE_ID,
  TOK_USER_HOOK_ID,
  TOK_PSEUDO_FRAG_EOF,
  TOK_RE_FRAG_ID,

  TOK_RANGE_SEP,

  TOK_RE_DOT,
  TOK_RE_SPACE_CLASS,
  TOK_RE_WORD_CLASS,
  TOK_RE_DIGIT_CLASS,
  TOK_RE_HEX_CLASS,

  TOK_RE_REF,

  TOK_PEG_ID,
  TOK_PEG_TOK_ID,
  TOK_TAG_ID,

  // *chars tokens must come last: MIN-RULE picks smallest action_id on conflict,
  // so specific tokens above must have lower values than these catch-all patterns.
  TOK_CODEPOINT,
  TOK_C_ESCAPE,
  TOK_PLAIN_ESCAPE,
  TOK_CHAR,

  TOK_COUNT
} TokenId;

#include "peg.h"
#include "token_tree.h"
#include "vpa.h"

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

  ReFrags re_frags;     // darray of ReIr, indexed by frag_id
  Symtab re_frag_names; // frag name -> frag_id

  VpaScope* vpa_scopes;
  EffectDecls effect_decls;
  Symtab tokens;      // unified token numbering, start from 1
  Symtab hooks;       // hook numbering, start from 1 (.begin=1, .end=2, .fail=3, .unparse=4, .noop=5)
  Symtab scope_names; // scope numbering, start from 1
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
