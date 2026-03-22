** OUT DATED, WILL RE-WORK THIS SOON **

An example show casing the capability and debuggability.

It is just for show casing, should be super simple, should be workable with pre-compiled distribution.

- `example/drive.c`: use the lex api to create LLVM IR `lex.ll`
  - syntax: a simple language with keywords (`if`, `else`, `while`),
    identifiers (`[a-zA-Z_]\w*`), numbers (`\d+`), whitespace (`\s+`),
    and single-char operators (`=`, `+`, `(`, `)`, `{`, `}`, `;`)
  - keep `lex.ll` in example folder for later inspect
  - use `__LINE__` to help resulting DebugInfo to locate regexp strings
- `example/main.c`:
  - use `src/ustr.h`'s interface to iterate source char by char
  - use generated `lex.ll` to lex string
- `example/build.sh`: to build example
  - for simplicity just cd to this folder to build, and outputs in this folder too
  - use `$CC` or just fallback to `clang`
  - can put int some custom flags

Lookahead-1 behavior

- as we feed characters to lexer, an action_id is emit, lexing shall:
  - keep previous emit action_id
  - when emits invalid action_id or all input is consumed, and previous action_id is positive, emit previous action_id an char position
