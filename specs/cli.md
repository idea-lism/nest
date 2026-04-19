Command line tool that generates 

- subcommands:
  - `nest l` simple lex
  - `nest c` generate parser by complete syntax
  - `nest h` show nest syntax help
    - lists `specs/nest_syntax.md`
  - `nest r` show nest grammar reference
    - lists `specs/bootstrap.nest`
- common options
  - `-t <target_triple>` specify target triple, if none given, probe clang's default triple
  - `-v 1`, or `--verbose=1`. verbose output (prints each step)
- parser generator options
  - `-m <memoize_mode>`, or `--memoize=<memoize_mode>`
    - `none`: parse not using memoize table, this mode is only for testing parser correctness, no memoize output.
    - `naive`: direct memoize table
    - `shared`: using row-shared memoize table, this is the default value.
  - `-p <prefix>`, or `--prefix=<prefix>`. generated functions must have `<prefix>_`
    - if not given, default to `"nest"`
    - prefix must match `[a-zA-Z]\w*`, with a limit of 64 chars.
    - to prevent conflicts: if prefix be any of `parse`, `ustr`, `load`, `lex`, report "prefix name preserved" and exit instantly.

### lex

- calling: `nest l input_file.txt -o output_file.ll -m <mode_flag> -f <function_name> -t <target_triple>`
- input file format:
  - each line is a regexp, auto assigning action_id (starting from 1)

### parser

- calling: `nest c <input.nest> -p <prefix> [-t <target_triple>] [-k false] [-v <level>]`
- output files (all written to the current directory, named after `-p`):
  - `<prefix>.ll` — LLVM IR for the lexer and parser
  - `<prefix>.h` — C header with token enums, scope defines, hook defines, parse result types, and inline PEG accessors
  - `<prefix>.c` — usage example (see below)
- invokes parse API to process.

#### generated `<prefix>.c`

A self-contained usage example that:
- `#define NEST_RT_IMPLEMENTATION` and `#include "<prefix>.h"`
- use `ustr_from_file` to read file.
- implements `int32_t <prefix>_next_cp(void* userdata)` for character reading -- userdata is a pointer to a struct, which includes a ustr iterator member and calls `ustr_iter_next` to get the cp or EOF.
- provides a `tok_name()` helper that maps every `TOK_*` id defined in the header to its lowercase name
- `main(argc, argv)` reads `argv[1]` as input, runs `vpa_lex`, iterates the token tree, and prints one line per token: `<token_name> (id=<id>) "<matched_text>"`

### examples

example 1, in examples/simple_lex, add tokens.txt and use `nest l` to generate lexer, add a `main.c` to use the lexer.

example 2, in examples/simple_nest, use `nest c` to compile a simple nest parser, supporting c-like integer arithmetics and simple control struct: `if () { ... }`, `else { ... }`, `while () { ... }`. each block should be a scope.

### example building

add a script for each example, with cc / cflags definition. which compiles the example (but not running it)

```sh
CC="${CC:-clang}"
CFLAGS="${CFLAGS:--std=c23 -O0 -g}"
```

the simple_nest build script should use the generated files directly:

```sh
nest c grammar.nest -p calc
cc calc.c calc.ll -o main
```

### examples gitingore

add `examples/.gitignore` to ignore user-built results

### readme

Add examples usage in README.md
