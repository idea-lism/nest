### Overview

Parser generator combining visibly pushdown automata (VPA) and parsing expression grammar (PEG).

- VPA is a set of minimized DFAs with a scope stack
  - it can emit nested token stream
  - it can be extended by hook predicates and dynamic state parsing
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

1. Create `src/header_writer.c` to make C header generating a bit easier.
2. implement a manual pushdown automata in `src/parse.c`, utilizing lexers defined by `src/parse_gen.c`.
3. implement recursive descendent parsers in `src/parse.c` to:
   - recursive descend parse the whole source
   - the token chunk helper lives in `src/token_chunk.c`
   - the regexp AST parsing is relatively independent, so it lives in `src/re_ast.c`
   - post-process:
     - expand the `%keyword` sugar
     - inline macro vpa rules
   - validate syntax correctness (specs below)
   - allocate peg rule ids for each scope
   - put processed content as `struct Parser` members
   - invoke `vpa_gen()` and `peg_gen()`, produce a combined header and a combined LLVM IR module.
4. create `src/vpa.c` (`vpa_gen()`):
   - define functions to generates visibly pushdown automata in LLVM IR, using Parser's processed-data
   - generate helpers for result C header
      1. token id definitions
      2. util functions that the final LLVM IR may need
5. create `src/peg.c` (`peg_gen()`):
   - define generation logic for different PEG constructs, and generate LLVM IR, using Parser's processed-data
   - generation helpers for result C header (reference the "Using the generated code" section below):
      1. node definition
      2. node extraction functions
      3. memoize table construction helpers for LLVM IR to use

Resulting interface:

- `parse_nest(const char* src, HeaderWriter* header_writer, IrWriter* ir_writer)`

For reference, file `specs/bootstrap.nest` contains the full syntax definition in its own syntax.

### The nested word `[[vpa]]`

Visibly pushdown (nested word) definition, served as lexer. Impl in `src/vpa.c`

Act as an low-level ambiguity resolver, we prefer first-declared state when possible.

The lexer will remain simple, just to solve the state and the macro problem.

Hooks have the info of current pos, parsed range, can return token id or break / fail.

to lex the syntax we mainly have these patterns defined:
- `%state` define state
- `%ignore` define tokens to ignore -- these tokens won't get into the stream
- `%effect` define effect of a hook. hooks without `%effect` won't emit token / break lexing loop
- `%keyword` define keyword token
- `.begin` primitive_hook: begin a scope (named by current rule name), later tokens will be pushed to a new chunk
- `.end` primitive_hook: end scope, back to parent token stream
- `.unparse` primitive_hook: put back the matched text so the next rule can re-match it
- `.fail` primitive_hook: fails parsing
- `[a-z_]\w*` id
- `\*{id}` macro rule, can be used inside a scope to inline definitions
- `\.{id}` hook_id
- `\@{id}` tok_id to emit
- `\${id}` state_id
- `(b|i|ib|bi)?\/` ... `/` regexp (must not be empty) -- should handle it with child DFA
- `["'']` ... `$last_quote` quoted string literal, also generates automata
- `=` assign
- `|` or
- `{` scope_begin
- `\n[[peg]]` ends the vpa parser, go to the peg parser
- comment, space, nl

First implementation of the syntax, must be a manual recursive descendant parser.

the semantic:
- `main` is the entrance
- `%keyword` is syntax sugar:
  - for example, `%keyword ops "=" "|"` expands to regexp rules `/=/ @ops.=` and `/|/ @ops.|`
    - then in PEG, `"="` is replaced with `@ops.=`, `"|"` with `@ops.|`
  - token names are `group.literal` — no identifier restrictions at the AST level
- `.on_*` hooks (e.g. `.on_tok_def`, `.on_id`, `.on_assign`) are user-defined semantic hooks referenced via `\.{id}` pattern. They fire during lexing for semantic actions (tracking definitions, resolving names, etc.) but don't affect the token stream unless combined with `%effect`.
- Visibly pushdown automata
  - scope
    - a union of vpa_rules
    - takes up a token in parent scope
  - lookahead-1 for greedy match: when there can be no action to emit, the last min action wins (see also the MIN-RULE in aut.md)
- `\@{id}` means put tok_id into the token streaming token tree (see below)
- when a vpa scope ends, if there is a `peg_rule` with the same id as the `vpa_rule`, invoke peg parsing on the scope tokens.
  - non-peg vpa's -- don't create sub-stream (see below example)
  - non-vpa peg's -- just a peg sub-rule, no special handling

token stream at runtime looks like this if `scope1` is used in peg but `scope2` is not used in peg.
```
tok1 tok2 scope1 tok7 tok8
            |
         tok3 tok4 (scope2 begins) tok5 tok6 (scope2 ends)
```

### The peg `[[peg]]`

Parsing expression grammar, Impl in `src/peg.c`, additional impl doc `specs/peg.md`.

tokens include:
- `[a-z_]\w*` if after a ":", assign tag id for this branch, else is PEG rule id
- `\@{id}` token id
- `: id` tag a branch
- `=` assign
- `[` branches_begin
- `]` branches_end
- `<` join_begin
- `>` join_end
- `?` maybe, greedy
- `+` plus, PEG possesive matching
- `*` star, PEG possesive matching
- keyword syntax sugar
- comment, space, nl as in vpa

branch tagging syntax
- each branch has a tag (denoted by `: the_tag`)
- if no tag given, it is auto-tagged with the first token / sub_rule name
- in a rule definition, tags must be distinct, or the parser will raise an error on conflict tag names
- epsilon branch can have a tag too, for example:

branch tagging example:

```conf
foo = a [
  @foo bar # not specifying, induce tag "foo" by first token
  sub_rule # not specifying, induce tag "sub_rule" by first sub rule
  @foo : tag2 # tag "foo" already used, must specify tag so there won't be conflict
  "a-keyword" : tag3 # tag can't be induced by keyword literal, so tag it manually
  : tag4             # epsilon branch needs to be tagged
]
```

When there are multiple branches in one rule, all branches are tagged. for example:

```conf
# resulting rule will have 4 branch tags b1 .. b4
foo = a [
  b1
  b2
] [
  b3
  b4
]
```

PEG semantics:
- if there's id named `main` , it is the entrance. there must be one entrance
- besides basic peg semantics, we have "join" semantics which interlaces the "operator" inside angle brackets with the base_multi_unit

### Nest file definition validation

- `main` must exist in `[[vpa]]`
- each scope in `[[vpa]]` must have a `.begin` (or user hook that produces the `.begin` effect) and one or more `.end` (or user hook that produces the `end` effect)
- for a same scope, used token set in `[[peg]]` must be the same as emit token set in `[[vpa]]`
  - for example, 
    - with vpa rule `foo = /.../ @a`, `bar = foo @b`, `bar`'s emit token set is `{@a, @b}` (including descendant's)
    - with peg rule `foo = @c?`, `bar = foo @b`, `bar`'s used token set is `{@c, @b}`, this doesn't equal to the vpa rule's token set
    - it is a mismatch, then we should raise error to tell user this rule doesn't add up

### Implemenation validations

- Token ids are allocated scopes too
- Recursive descend parsers work on scope level
  - the input token stream buffer is a scoped chunk
  - no expand sub-scope parsing -- sub-scopes are just a token_id match
- String literals are stored with source offset + length, no extra allocations for them
- Regexps are converted to structured AST that can be used by `src/vpa.c`
- When a scope starts, it should allocate a new token stream chunk.
- When a scope is complete (at `.end` hook), it should invoke recursive descend parsing on the token stream chunk

### What is FORBIDDEN, a no-go

- Re-inventing regexp/lexer machine in parse.c
- Stub implementations
