# nest

Regex-to-LLVM-IR compiler and PEG parser generator.

## Build

Requires Ruby, [Ninja](https://ninja-build.org/), and `unzip`.

```
ruby config.rb debug    # or: ruby config.rb release
ninja
```

## Test

```
scripts/test
```

## Usage

### Lexer generation

Given a file with one regex per line (`tokens.txt`):

```
if
else
while
[a-zA-Z_]\w*
\d+
\s+
```

Generate LLVM IR:

```
nest l tokens.txt -o lex.ll
```

Each line is assigned an `action_id` starting from 1. The generated function has the signature `(i32 state, i32 codepoint) -> {i32 new_state, i32 action_id}`.

Options:

- `-f <name>` -- function name (default: `lex`)
- `-m <mode>` -- `i` for case-insensitive, `b` for binary
- `-t <triple>` -- target triple (default: probe clang)

### Parser generation

Compile a `.nest` syntax file into LLVM IR and a C header:

```
nest c grammar.nest -o parser.ll
```

This produces `parser.ll` and `parser.h`.

## Examples

### Standalone lexer (`examples/simple_lex`)

Generates a longest-match lexer from plain regex patterns.

```
cd examples/simple_lex
./build.sh
./main "if x while 42"
```

### Full parser (`examples/simple_nest`)

Compiles a `.nest` grammar (VPA lexer + PEG parser) supporting C-like integer
arithmetic with `if`/`else`/`while` control structures.

```
cd examples/simple_nest
./build.sh
./main "while (x + 1) { y = 42; }"
```

Both build scripts default to the debug build of `nest`. Override with
`NEST=path/to/nest`, `CC`, or `CFLAGS` environment variables.
