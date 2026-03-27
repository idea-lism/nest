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

## Example

```
cd example
./build.sh
./main "while (x + 1) { y = 42; }"
```
