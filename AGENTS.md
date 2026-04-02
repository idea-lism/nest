# Project: tailorbird

## Brief

1. A regex-to-LLVM-IR compiler.
   - Regex patterns -> NFA -> DFA (minimized) -> LLVM IR text -> native code via clang.
2. A optimized PEG parser.
   - Graph-coloring optimized parsing.

## Rules

- If the user says "refactor", treat that as permission for disruptive changes.
- Prefer small, local edits over broad rewrites.
- Do not invent compatibility layers unless there is a real external need.
- Check the relevant `specs/*.md` file before changing a subsystem.

## Key Entry Points

- `src/nest.c`: CLI entry point. Subcommands: `nest l`, `nest c`, `nest h`, `nest r`.
- `src/re.c`, `src/aut.c`, `src/irwriter.c`: regex -> automata -> LLVM IR pipeline.
- `src/parse_gen.c`: build-time generated lexers for `.nest` syntax.
- `src/parse.c`: parses `.nest` sources and owns parser state/error reporting.
- `src/peg.c`, `src/peg_ir.c`: PEG analysis and code generation.
- `src/vpa.c`: visibly pushdown lexer/parser pieces.
- `src/coloring.c`, `src/graph.c`: graph coloring for PEG memoization layout.
- `src/ustr.c`, `src/bitset.c`, `src/darray.c`: reusable runtime/data structures.

## Repo Map

- `src/` core implementation.
- `test/` tests and benchmarks.
- `scripts/` wrappers for test, bench, and coverage runs.
- `specs/` design docs; read the relevant one before editing.
- `config.rb` / `config.in.rb`: generate `build.ninja` and define targets.
- `README.md`: user-facing build and CLI usage.

## Specs To Read First

- Regex / automata work: `specs/re.md`, `specs/aut.md`, `specs/re_ir.md`, `specs/irwriter.md`
- Parser frontend work: `specs/parse.md`, `specs/parse_gen.md`, `specs/post_process.md`
- PEG work: `specs/peg.md`, `specs/peg_ir.md`, `specs/coloring.md`
- CLI / build / tests: `specs/cli.md`, `specs/building.md`, `specs/test.md`
- Strings / containers: `specs/ustr.md`, `specs/bitset.md`, `specs/darray.md`

## Build

Prereqs: Ruby, Ninja, and `unzip`.

Common build:

```sh
ruby config.rb debug
ninja
```

```sh
ruby config.rb release
ninja
```

```sh
ruby config.rb coverage
ninja
```

Build modes from `config.in.rb`:

- `debug`: `-O0 -g -fsanitize=address -fsanitize=undefined`
- `release`: `-O2`
- `coverage`: `-O0 -g -fprofile-instr-generate -fcoverage-mapping`

- `build/<mode>/nest`
- `build/<mode>/parse_gen`
- `build/<mode>/test_*`
- `build/<mode>/bench_*`
- `out/libre.a`
- `out/rt.h`

## Test And Benchmark Commands

```sh
scripts/test
```

```sh
scripts/test debug
scripts/test release
```

Single test flow:

```sh
ruby config.rb debug
ninja build/debug/test_aut
build/debug/test_aut
```

Common single-test binaries:

```sh
build/debug/test_re
build/debug/test_parse
build/debug/test_peg
build/debug/test_coloring
```

```sh
scripts/bench
scripts/bench release
build/release/bench_ustr
```

```sh
scripts/coverage
```

- `build/coverage/html/index.html`

Useful target discovery:

```sh
ninja -t targets | grep 'test_'
```

## Formatting

Clang-format is configured by `.clang-format`:

- based on LLVM style
- column limit `120`
- pointer alignment: left (`foo* p`)
- braces are inserted for block bodies

```sh
xcrun clang-format -i path/to/file.c path/to/file.h
```

On other platforms use `clang-format -i`.

## Code Style

- Types use camel case: `Aut`, `IrWriter`, `ColoringResult`.
- Variables and functions use snake case: `header_path`, `aut_gen_dfa`.
- Static private helpers start with `_`: `_usage`, `_detect_triple`, `_build_segments`.
- Use explicit stdint types: `int32_t`, `uint8_t`, `int64_t`.
- Prefer `bool` for boolean state where the module already uses it.
- Keep related logic in one function unless extraction clearly improves reuse or clarity.
- Prefer small local structs and helpers over large abstraction layers.

## Includes And File Layout

- Put project headers first, then system headers.
- Keep include order stable and simple; do not over-normalize unrelated files.
- Most `.c` files define private structs and static helpers near the top.
- Public API belongs in headers under `src/`; internal-only helpers stay `static` in the `.c` file.

## Error Handling Conventions

- Library-style functions typically return `NULL`, `false`, or negative error codes on failure.
- CLI-facing code prints diagnostics with `perror` or `fprintf(stderr, ...)` and returns non-zero.
- Parser code centralizes user-facing parse errors in `ParseState` via `parse_error()` / `_error_at()`.
- Do not hide failures with fallback behavior unless the existing code already does that intentionally.
- Clean up owned resources on error paths; this codebase usually frees explicitly before returning.
- Follow existing sentinel conventions such as `0` meaning unset action and negative values for special errors.

## Testing Style

- Tests are plain C executables under `test/`.
- Common pattern: `#define TEST(name) static void name(void)` and `RUN(name)` in `main`.
- Assertions use standard `assert()` heavily.
- Prefer adding or updating the smallest test binary covering your change.
- If you touch a generator, test both structure and generated text where practical.

## Project-Specific Notes

- The generated lexer function signature is `(i32 state, i32 codepoint) -> {i32 new_state, i32 action_id}` and is widened to `i64` pairs for the C ABI.
- Special codepoints are `BOF = -1` and `EOF = -2`.
- Regex conflict resolution uses MIN-RULE semantics for action IDs.
- On macOS/Linux, `kissat` is built automatically; on Windows, `coloring.c` falls back to DSatur.
- Some older docs or agent notes may mention `lex.c` / `ulex.c`; current CLI entry point is `src/nest.c`.

## Agent Checklist

- Read the relevant spec before editing a subsystem.
- Build the smallest relevant target first.
- Run the narrowest relevant test binary, then broader tests if needed.
- Format touched C/C header files.
- Avoid changing unrelated files or rewriting large generated/derived sections without need.

## External Agent Rules

- No `.cursor/rules/`, `.cursorrules`, or `.github/copilot-instructions.md` files exist in this repository at the time of writing.
