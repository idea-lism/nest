Command line tool that generates 

- `-h` show help
- subcommands:
  - `nest l` simple lex
  - `nest c` generate parser by complete syntax
  - `nest h` show help `specs/nest_syntax.md`
  - `nest r` show reference `specs/bootstrap.nest`
- common options
  - `-t <target_triple>` specify target triple, if none given, probe clang's default triple
- parser generator options
  - `-k false`, or `--compress-memoize=false`. default true, set false to disable compress memoize table (row_shared mode in PEG)

### lex

- calling: `nest l input_file.txt -o output_file.ll -m <mode_flag> -f <function_name> -t <target_triple>`
- input file format:
  - each line is a regexp, auto assigning action_id (starting from 1)

### parser

- invokes parse API to process.

### examples

example 1, in examples/simple_lex, add tokens.txt and use `nest l` to generate lexer, add a `main.c` to use the lexer.

example 2, in examples/simple_nest, use `nest c` to compile a simple nest parser, supporting c-like integer arithmetics and simple control struct: `if () { ... }`, `else { ... }`, `while () { ... }`. each block should be a scope.

### example building

add a script for each example, with cc / cflags definition. which compiles the example (but not running it)

```sh
CC="${CC:-clang}"
CFLAGS="${CFLAGS:--std=c23 -O0 -g}"
```

### examples gitingore

add `examples/.gitignore` to ignore user-built results

### readme

Add examples usage in README.md
