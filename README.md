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
scripts/test            # release mode
scripts/test debug      # debug mode with sanitizers
scripts/test_examples   # run .nest example files
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

Each line is assigned an `action_id` starting from 1. The generated function has the signature `(i64 state, i64 codepoint) -> {i64 new_state, i64 action_id}`.

Options:

- `-f <name>` — function name (default: `lex`)
- `-m <mode>` — `i` for case-insensitive, `b` for binary
- `-t <triple>` — target triple (default: probe clang)

### Parser generation

Compile a `.nest` syntax file into LLVM IR, a C header, and a usage example:

```
nest c grammar.nest -p calc
```

This produces `calc.ll`, `calc.h`, and `calc.c` in the current directory.

Options:

- `-p <prefix>` — generated function prefix (required; must match `[a-zA-Z]\w*`, max 64 chars)
- `-m <mode>` — memoize mode: `none`, `naive`, `shared` (default: `shared`)
- `-t <triple>` — target triple (default: probe clang)
- `-v <level>` — verbosity level

The generated `<prefix>.c` usage example:

- Defines a `<prefix>_next_cp(void* userdata)` callback that reads codepoints via a `UstrIter` inside the userdata struct
- Provides a `tok_name()` helper mapping every `TOK_*` id to its name
- `main()` reads input, runs `vpa_lex`, iterates the token tree, and prints one line per token

### Syntax reference

```
nest h    # show .nest syntax reference
nest r    # show bootstrap.nest reference
```

## Examples

### Standalone lexer (`examples/simple_lex`)

Generates a longest-match lexer from plain regex patterns.

```sh
cd examples/simple_lex
./build.sh
./main "if x while 42"
```

### Full parser (`examples/simple_nest`)

Compiles a `.nest` grammar (VPA lexer + PEG parser) supporting C-like integer
arithmetic with `if`/`else`/`while` control structures.

```sh
cd examples/simple_nest
./build.sh
./main "while (x + 1) { y = 42; }"
```

Both build scripts default to the debug build of `nest`. Override with
`NEST=path/to/nest`, `CC`, or `CFLAGS` environment variables.

## Benchmarks

Benchmarks compare nest against [PackCC](https://github.com/arithy/packcc) and [tree-sitter](https://tree-sitter.github.io/) across calc, json, and kotlin grammars.

### Prerequisites

Requires a release build of nest:

```sh
ruby config.rb release
ninja
```

### Quick run

```sh
benchmark/benchmark
```

This generates inputs, runs all benchmarks (internal + comparison), and produces markdown reports in `benchmark/results/`.

### Individual steps

```sh
ruby benchmark/run.rb setup      # install competitors (packcc, tree-sitter)
ruby benchmark/run.rb gen        # generate internal benchmark inputs
ruby benchmark/run.rb internal   # nest-only ablation: memoize modes × optimization levels
ruby benchmark/run.rb compare    # nest vs packcc vs tree-sitter
ruby benchmark/run.rb report     # generate markdown reports from CSVs
ruby benchmark/run.rb all        # gen + internal + compare + report
```

### Internal benchmarks

Nest-only comparison across:

- **Memoize modes:** `none`, `naive`, `shared`
- **Optimization:** `-O0`, `-O2`
- **Input sizes:** 1KB, 10KB, 100KB, 1MB

Metrics: parse time, throughput (MB/s), peak RSS, token/chunk counts, memoize table size, lex vs parse time split.

### Comparison benchmarks

Cross-framework on identical grammars/inputs (from PackCC benchmark repo):

- **nest** (shared memoize, `-O2`)
- **PackCC** (PEG-to-C)
- **tree-sitter** (GLR)

Metrics: parse throughput, peak RSS, grammar compile time, binary size.
