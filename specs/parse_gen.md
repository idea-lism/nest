build lexer on top of re API, for bootstraping parser only.

## File

src/parse_gen.c

## Lex helpers

- basic functions: `_lex_new(source_file_name, mode)` and `lex_del()`
  - `mode` is combination of following chars or just empty string "":
    - "i": ignore case
    - "b": binary mode (by default it is UTF-8 mode, must use src/ustr.h to init the UTF-8 string and do the parse)
- parse regexp and add branch: `_lex_add(l, pattern_string, __LINE__, /* col */ 15, action_id)`
  - a recursive descend parser, handles following syntax and translate to regexp constructs (see src/re.h):
    - boundaries: `\a` for beginning of input, `\z` for end of input
    - char class: `[0-9a-z]`, negative char class like: `[^a-z]`
    - C-escapes: `\n`, `\t`, ...
    - Normal char escapes: `\-` means `-`, this is useful in char class
    - unicode escapes: `\u{3457}`
    - special char classes: only `.`, `\s`, `\w`, `\d`, `\h`
    - branching: `foo|bar`
    - grouping: `(some_group)`
    - multi qualifiers: `e?`, `e+`, `e*`
    - no word boundaries -- it requires lookahead-2 mode DFA
  - when parsing failed, `_lex_add()` returns negative error code:
    - paren not match
    - char group not closed
  - when parsing good, return action id
- `_lex_gen(FILE*)` create the lexer function in LLVM IR

## Parse gen

Create `src/parse_gen.c`, which compiles to a build-time cmd
- mimic the struct of `specs/bootstrap.nest`, but with hand-written lex helper calls
- uses the lex helpers to generate DFAs (one DFA per scope) for the whole source file
- use the token definition in `src/parse.h` for lex actions.

Regexps that having an `.unparse .end` hook should generate a `TOK_UNPARSE_END` after it.

Regexps that having an `.end` hook should generate a universal `TOK_END`.

Regexps that are not generating tokens can generate a universal `TOK_IGNORE`.

When a subscope is there, match the first regexp of the subscope, and emit a `SCOPE_XXX` token.

The tokens are defined in [src/parse.h](src/parse.h) as:

```c
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
  SCOPE_STR,

  SCOPE_COUNT
} ScopeId;

typedef enum {
  ACTION_START = SCOPE_COUNT,

  ACTION_IGNORE, // .ignore
  ACTION_BEGIN, // .begin
  ACTION_END, // .end
  ACTION_UNPARSE, // .unparse
  ACTION_FAIL, // .fail
  ACTION_STR_CHECK_END, // .str_check_end

  // composite: since lexer api only accepts single action_id, multiple actions must be combined
  ACTION_UNPARSE_END, // .unparse .end
  ACTION_SET_RE_MODE_BEGIN, // .set_re_mode .begin
  ACTION_SET_CC_KIND_BEGIN, // set_cc_kind .begin
  ACTION_SET_QUOTE_BEGIN, // .set_quote .begin
  ACTION_RE_TAG_BEGIN, // @re_tag .begin
  ACTION_CHARCLASS_BEGIN_BEGIN, // @charclass_begin .begin

  ACTION_COUNT
} ActionId;

typedef enum {
  LIT_START = ACTION_COUNT,

  LIT_IGNORE, // "%ignore"
  LIT_EFFECT, // "%effect"
  LIT_DEFINE, // "%define"
  LIT_EQ, // "="
  LIT_OR, // "|"
  LIT_INTERLACE_BEGIN, // "<"
  LIT_INTERLACE_END, // ">"
  LIT_QUESTION, // "?"
  LIT_PLUS, // "+"
  LIT_STAR, // "*"
  LIT_LPAREN, // "("
  LIT_RPAREN, // ")"

  LIT_COUNT
} LitId;

typedef enum {
  TOK_START = (1 << 16), // large enough first token ID so it won't conflict with scope_id

  // shared tokens
  TOK_NL,

  // scope: vpa
  TOK_TOK_ID,
  TOK_HOOK_BEGIN,
  TOK_HOOK_END,
  TOK_HOOK_FAIL,
  TOK_HOOK_UNPARSE,
  TOK_VPA_ID,
  TOK_MODULE_ID,
  TOK_USER_HOOK_ID,
  TOK_STATE_ID,
  TOK_RE_FRAG_ID,

  // shared by re, charclass, str
  TOK_CODEPOINT,
  TOK_C_ESCAPE,
  TOK_PLAIN_ESCAPE,
  TOK_CHAR,

  // str scope
  TOK_STR_START,

  // re scope
  TOK_RE_DOT,
  TOK_RE_SPACE_CLASS,
  TOK_RE_WORD_CLASS,
  TOK_RE_DIGIT_CLASS,
  TOK_RE_HEX_CLASS,

  // re_ref scope
  TOK_RE_REF,

  // charclass scope
  TOK_RANGE_SEP,

  // peg scope
  TOK_PEG_ID,
  TOK_PEG_TOK_ID,
  TOK_TAG_ID,

  TOK_COUNT
} TokenId;
```
