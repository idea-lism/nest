### Overview

Parser generator combining visibly pushdown automata (VPA) and parsing expression grammar (PEG).

- VPA is a set of minimized DFAs with a scope stack
  - it can emit nested token stream
  - it can be extended by hook predicates
- PEG work on the token level, not character level
  - PEGs are separated / nested by the corresponding VPA scope
  - so for each child PEG the rule count is much smaller

File format:

```conf
[[vpa]]

# comment: define vpa_rules
...

[[peg]]

# comment: define peg_rules
```

The overall handling with this syntax:

1. implement a manual pushdown automata in `src/parse.c`, utilizing lexers defined by `src/parse_gen.c`.
2. implement recursive descend parsers in `src/parse.c` to:
   - recursive descend parse the whole source
   - utilize [token chunk](src/token_chunk.h)

Resulting interface:

- `bool parse_nest(const char* ustr_src, HeaderWriter* header_writer, IrWriter* ir_writer)`
  - `ustr_src` must be a ustr

For reference, file `specs/bootstrap.nest` contains the full syntax definition in its own syntax.

### The nested word `[[vpa]]`

Visibly pushdown (nested word) definition, served as lexer. Impl in `src/vpa.c`

Act as a low-level ambiguity resolver, we prefer first-declared action when possible.

The lexer will remain simple, just to solve the state and the macro problem.

Hooks have the info of current pos and parsed range in `Token tok`, can return token id or primitive hook (details in [vpa](vpa.md)).

Some tokens and their meanings (details in bootstrap.nest):
- `%ignore` define tokens to ignore -- these tokens won't get into the stream
- `%effect` (for validation-only) define effect of a hook. hooks without `%effect` won't emit token / end lexing loop
- `%define` creates re_frag database, when parsing, expand defines
- `.begin` primitive_hook: begin a scope (named by current rule name), later tokens will be pushed to a new chunk
- `.end` primitive_hook: end scope, back to parent token stream
- `.unparse` primitive_hook: put back the matched text so the next rule can re-match it
- `.fail` primitive_hook: fails parsing
- `ID` scope id
- `\*{ID}` macro rule, can be used inside a scope to inline definitions
- `\.{ID}` hook_id
- `\@{ID}` tok_id to emit, means put tok_id into the token streaming token tree (see below)
- `(b|i|ib|bi)?\/` ... `/` regexp (must not be empty) -- should handle it with child DFA
- `.` sets `.` quoted string literal, also generates automata
- `{` begins a scope
- `@{` begins a literal module, which is syntax sugar which will be expanded at [post process](post_process.md)
  - For each string literal, auto-create token and [re_ir](re_ir.md).
    - for example, `@{ "+" }` auto-creates token `@lit.+` and a regexp IR that recognizes `"+"`.
- `\n[[peg]]` unparse the token, and ends the vpa parser, then the main lexing will start the peg scope

First implementation of the syntax, must be a manual recursive descend parser.

More on the semantics (see also validations in [post_process](post_process.md)):
- `main` is the entrance
- user_hook_ids are user-defined functions referenced via `\.{ID}` pattern. They fire during lexing for semantic actions (tracking definitions, resolving names, etc.) but don't affect the token stream unless combined with `%effect`.
- Visibly pushdown automata
  - scope
    - a union of vpa_rules
    - if a peg is defined on it, takes up a token in parent scope
    - if there's no peg defined on it, flattend in parent scope
  - lookahead-1 for greedy match: when there can be no action to emit, the last min action wins (see also the MIN-RULE in aut.md)
- when a vpa scope ends, if there is a `peg_rule` with the same id as the `vpa_rule`, invoke peg parsing on the scope tokens.
  - both peg & vpa -- create sub-stream and invoke parsing on sub-stream
  - non-peg vpa -- don't create sub-stream (see example below)
  - non-vpa peg -- just a peg sub-rule, no special handling

in addition, the `re` scope has interpolation syntax:
- `#{UPPER_CASED_ID}` references a predefined regexp fragment

token stream at runtime looks like this if `scope1` is used in peg but `scope2` is not used in peg.
```
tok1 tok2 scope1 tok7 tok8
            |
         tok3 tok4 (scope2 begins) tok5 tok6 (scope2 ends)
```

### The peg `[[peg]]`

Define parsing expression grammar

Some tokens and their meanings (details in bootstrap.nest):
- `[a-z_]\w*` if after a ":", assign tag id for this branch, else is PEG rule id
- `\@{ID}` token id
- `:` ... then `ID`: tag a branch
- `[` branches_begin
- `]` branches_end
- `<` join/interlace begin
- `>` join/interlace end
- `?` maybe, greedy
- `+` plus, PEG possesive matching
- `*` star, PEG possesive matching
- string literals, replace them with auto-generated token the same way as in vpa parser.
  - for example, `"+"` auto-creates token `@lit.+`.

More on the semantics (see also validations in [post_process](post_process.md)):
- if there's id named `main` , it is the entrance. there must be one entrance
- besides basic peg semantics, we have "join/interlace" semantics which interlaces the "operator" inside angle brackets with the base_multi_unit
  - to get the idea, can reference `chain` rule Haskell's Parsec library

branch tagging syntax
- each branch has a tag (denoted by `: the_tag`)
- if no tag given, it is auto-tagged by [post_process](post_process.md).

### Nested word lexing

in the bootstraping `parse.c`, define `bool _lex_scope(ctx, scope_id)` function, which finds corresponding scope config and lex.

```c
// darray of ScopeConfig
typedef ScopeConfig* ScopeConfigs;

struct ScopeConfig {
  const char* scope_name;
  int32_t scope_id;
  LexFunc lex_fn;
  ParseFunc parse_fn;
}
```

In the loop of `_lex_scope`
- when met `end_token`, return and pop chunk head.
- when a token_id is a scope_id (tok_id <= SCOPE_MAX), call `_lex_scope(ctx, tok_id)`.

Error handling:
- when sub lexer meets error, set error to lexing state
- parent lexer checks lexing state after calling sub lexer, if error, just return

The [token stream tree](src/token_chunk.c) is organized the same way as `TokenTree` in [VPA generated code](specs/vpa.md).

The parsing also follows this nested structure:
- When parsing matches to a rule that is not a scope, call it as in normal recursive descend parsers
- When parsing matches to a rule that maps to a scope, read a scope_id token and the chunk it points to, and call rule with the new child chunk

In summary, only `ACTION_XXX` require special handling in the lexing loop:

```c
// single
ACTION_IGNORE, // .ignore
ACTION_BEGIN, // .begin
ACTION_END, // .end
ACTION_UNPARSE, // .unparse
ACTION_FAIL, // .fail
ACTION_STR_CHECK_END, // .str_check_end

// composite: since lexer api only accepts single action_id, multiple actions must be combined
ACTION_UNPARSE_END, // .unparse .end
ACTION_SET_QUOTE_BEGIN, // .set_quote .begin
ACTION_RE_TAG_BEGIN, // @re_tag .begin
ACTION_CHARCLASS_BEGIN_BEGIN, // @charclass_begin .begin
```

For detailed scope/action/lit/tok see [parse_gen](parse_gen.md).

So after we called a generated `lex_xxx()` and met a mismatch / eof, we check the `last_action_id`:

```c
bool _lex_scope(ctx, scope_id) {
  ScopeConfig cfg = configs[scope_id];
  while (take a codepoint from input) {
    if (scope has eof) {
      // the scope has EOF (@pseudo_frag_eof)
      ... check if we are at eof and invoke aciton if needed
    }
    action_id = feed codepoint to current lexer
    if (action_id is mismatch) {
      if (last_action_id > 0) {
        if (last_action_id < SCOPE_COUNT) {
          _lex_scope(ctx, last_aciton_id); // call down the lexer
        } else if (last_action_id < ACTION_COUNT) {
          switch (last_action_id) {
            case ACTION_IGNORE:
              // do nothing
            case ACTION_BEGIN:
              create new token chunk at current pos
            case ACTION_END:
              goto done
            case ACTION_UNPARSE:
              revert one token
            case ACTION_SET_QUOTE_BEGIN:
              ctx->last_quote_cp = decode_cp;
              goto ACTION_BEGIN;
            case ACTION_STR_CHECK_END:
              if (decode_cp == ctx->last_quote_cp) {
                goto ACTION_END;
              } else {
                emit char token
              }
            case ...
          }
        } else if (last_action_id < LIT_COUNT) {
          emit literal token in token chunk
        } else {
          emit token in token chunk
        }
      } else {
        error
        return false
      }
    } else {
      continue feeding
    }
  }
done:
  // now the chunk is finished, call the parse function
  cfg.parse_fn(src, token_chunk);
  return true;
}
```

Scope parsing example, especially for peg_str & re_str:

```c
typedef void* DStr; // darray of chars

bool _parse_peg_str(const char* src, TokenChunk* chunk, int32_t* tpos) {
  DStr buf = darray_new(sizeof(int32_t), 0);
  char* b = (char*)buf;
  int32_t n = (int32_t)darray_size(chunk->tokens);
  for (int32_t i = *tpos; i < n; i++) {
    Token* t = &chunk->tokens[i];
    switch (t->tok_id) {
      case TOK_CHAR: {
        UstrCpBuf slice = ustr_slice_cp(src, t->cp_start);
        _push_buf(buf, slice.buf);
        break;
      }
      case TOK_CODEPOINT: {
        UstrIter it = {0};
        ustr_iter_init(&it, src, t->cp_start + 1);
        int32_t cp = re_hex_to_codepoint(src + it.byte_off, t->cp_size - 1);
        char slice[4] = {0};
        ustr_encode_utf8(slice, cp);
        _push_buf(buf, slice);
        break;
      }
      case TOK_C_ESCAPE: {
        UstrIter it = {0};
        ustr_iter_init(&it, src, t->cp_start + 1);
        darray_push(b, re_c_escape(src[it.byte_off]));
        break;
      }
      case TOK_PLAIN_ESCAPE: {
        UstrCpBuf slice = ustr_slice_cp(src, t->cp_start + 1);
        _push_buf(buf, slice.buf);
        break;
      }
    }
  }
  darray_push(b, '\0');

  chunk->value = buf;

  // there's no way to generate an invalid string, it just returns true
  return true;
}

typedef struct {
  DStr* tok_name;
  ReIr re_ir;
} ReStrUnit;

bool _parse_re_str(const char* src, TokenChunk* chunk, int32_t* tpos) {
  ReStrUnit unit = {0};
  // save some work by invoking existing parser
  int32_t tpos_copy = *tpos;
  _parse_peg_str(src, chunk, &tpos_copy);
  unit.tok_name = chunk->value;

  ReIr re = re_ir_new();
  int32_t n = (int32_t)darray_size(chunk->tokens);
  for (int32_t i = *tpos; i < n; i++) {
    Token* t = &chunk->tokens[i];
    switch (t->tok_id) {
      case TOK_CHAR: {
        int32_t cp = ustr_cp_at(src, t->cp_start);
        re = re_ir_emit_ch(re, cp);
        break;
      }
      case TOK_CODEPOINT: {
        UstrIter it = {0};
        ustr_iter_init(&it, src, t->cp_start + 1);
        int32_t cp = re_hex_to_codepoint(src + it.byte_off, t->cp_size - 1);
        re = re_ir_emit_ch(re, cp);
        break;
      }
      case TOK_C_ESCAPE: {
        UstrIter it = {0};
        ustr_iter_init(&it, src, t->cp_start + 1);
        char c = re_c_escape(src[it.byte_off]);
        re = re_ir_emit_ch(re, c);
        break;
      }
      case TOK_PLAIN_ESCAPE: {
        int32_t cp = ustr_cp_at(src, t->cp_start + 1);
        re = re_ir_emit_ch(re, cp);
        break;
      }
    }
  }

  chunk->value = malloc(sizeof(ReStrUnit));
  *(ReStrUnit*)chunk->value = unit;
  return true;
}
```

### Recursive descend parsing

- Token ids are allocated scopes too
- Recursive descend parsers work on scope level
  - the input token stream buffer is a scoped chunk
  - no expand sub-scope parsing -- sub-scopes are just a token_id match
- Regexps are converted to [IR](re_ir.md), a flattened representation of AST. Since regexp scopes are parsed BEFORE the vpa scope, we have to add the frag_ref in re_ir. Then in vpa's parsing, we execute the IR, inlining the frag_ref by lookup.
- When a scope is complete (at `.end` hook), it should invoke recursive descend parsing on the token stream chunk

### Regexp parsing and VPA parse result

When met `re_ref` sub-scope, insert frag_ref into re_ir, by looking up / create `Symtab re_frag_names` for the index.

The `%define` directives sets the `ReIr` item into the `ReFrags frags`, by looking up / create `Symtab re_frag_names` for the index.
- Note that `%define` also checks that id should not equal to `EOF`

Then in `re_ir_validate(...)` call, we pass the `frags` darray for the lookup and report regexp syntax errors.

### Token & Hook numbering

ATTENTION: this numbering system is for produced AST, don't confuse it with the bootstrap parser's design in [parse gen](parse_gen.md).

Parser should produce what vpa_gen / peg_gen requires.

- vpa_gen() & peg_gen() both needs tok_id, so we have symtab `ParseState.tokens` (num start from 1) for the uniq numbering
- user defined needs hook_id, so we have symtab `ParseState.hooks` (num start from 0) for user-defined hook numbering
  - the first enties are fixed for builtin hooks: .begin = 0, .end = 1, .fail = 2, .unparse = 3
- unified action_unit_id (see also `action` in [bootstrap syntax](bootstrap.nest)):
  - action_unit_id > 0: maps to token_id
  - action_unit_id <= 0: maps to -hook_id

### Misc

- re_str stores ReIr in `token_chunk.value`, and the transformed string buffer in `token_chunk.aux_value`
- peg_str stores transformed string buffer in `token_chunk.aux_value`

### What is FORBIDDEN, a no-go

- Re-inventing regexp/lexer machine in parse.c
- Stub implementations
