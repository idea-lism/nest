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

1. Create `src/parse.h` to define all token ids (action_id in automata), also scope ids are token ids.
2. Create `src/parse_gen.c`, which compiles to a build-time cmd
  - mimic the struct of `specs/bootstrap.nest`, but with hand-written lex helper calls
  - uses the lex helpers to generate DFAs (one DFA per scope) for the whole source file
  - use the token definition in `src/parse.h` for lex actions.
