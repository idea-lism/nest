# Nest Performance Benchmark Plan

## Assumptions

- **macOS only.** No Linux/Windows portability. Use `xcrun clang`, `brew`, `/usr/bin/time -l`, `mach_absolute_time`, `ru_maxrss` (bytes, not KB).
- **Homebrew available.** `setup.rb` uses `brew install` where possible.
- **Ruby only.** No Python. Scripts use stdlib only (no gems beyond ERB).
- **`build/release/nest` already built.** Benchmarks assume `ruby config.rb release && ninja` was run. Don't rebuild nest itself.
- **Internal benchmark inputs deterministic.** Seeded PRNG, committed to git so nest-only ablation runs are reproducible without regeneration.
- **Comparison inputs/grammars come from PackCC benchmark repo.** For cross-framework comparison, use PackCC benchmark grammars and inputs verbatim instead of generated data.
- **No incremental/partial runs.** Each subcommand runs its full scope. No resume logic.
- **Memory via `/usr/bin/time -l`.** External measurement. Runners don't need `getrusage` — the orchestrator wraps with `/usr/bin/time -l` and parses `maximum resident set size` (bytes on macOS).
- **Timing via `clock_gettime(CLOCK_MONOTONIC)` inside runners.** Each runner prints CSV to stdout. Orchestrator captures it.
- **tree-sitter installed via `brew install tree-sitter`.** The CLI and library come from Homebrew, no cargo needed.

## Structure

```
benchmark/
  spec.md              # this file
  setup.rb             # install competitors (packcc, tree-sitter)
  run.rb               # main benchmark orchestrator
  grammars/
    calc/
      grammar.nest     # nest version
      grammar.peg      # packcc version (from packcc repo)
      grammar.js        # tree-sitter version
    json/
      grammar.nest
      grammar.peg
      grammar.js
    kotlin/
      grammar.nest     # nest equivalent of kotlin.peg
      grammar.peg
      grammar.js        # tree-sitter-kotlin
  inputs/
    gen_inputs.rb      # deterministic generator for internal benchmarks only
    internal/
      calc/            # generated: xs(1K) sm(10K) lg(1M)
      json/
      kotlin/
    compare/           # copied from PackCC benchmark repo
  runners/
    nest_runner.c.erb  # template: compile-time specialized per grammar
    packcc_runner.c    # generic: packcc reads stdin
    treesitter_runner.c # generic: tree-sitter API
  results/             # output CSVs + reports
  internal/            # nest-only detailed benchmarks (memoize comparison etc.)
    run_internal.rb
```

## Competitors

### PackCC
- Direct PEG-to-C. Closest to nest's approach.
- Aligned grammars: calc.peg, json.peg, kotlin.peg (from packcc repo).
- Build: `cc -O2 packcc.c -o packcc && packcc -o parser grammar.peg && cc -O2 parser.c -o parser`

### tree-sitter
- GLR + error recovery. Industry standard.
- Install: `brew install tree-sitter`.
- Build: `tree-sitter generate && xcrun clang -O2 src/parser.c -I... -ltree-sitter -o parser`
- Grammars: tree-sitter-json (official), tree-sitter-kotlin from `https://github.com/tree-sitter-grammars/tree-sitter-kotlin`, calc (write small one).

## Grammars

### calc (small, recursion-heavy)
- Arithmetic expressions with +, -, *, /, unary, parens.
- ~10 rules. Tests deep recursion, operator precedence.
- PackCC: `benchmark/grammars/calc.peg` verbatim.

### json (medium, balanced)
- Objects, arrays, strings, numbers, booleans, null.
- ~10 rules. Tests breadth (wide arrays/objects) and depth (nested objects).
- PackCC: `benchmark/grammars/json.peg` verbatim.

### kotlin (large, real-world)
- Full Kotlin grammar subset (~200+ rules).
- Tests grammar compilation time, table sizes, memoization pressure.
- PackCC: `benchmark/grammars/kotlin.peg` verbatim.
- Nest: hand-translated `.nest` equivalent.
- tree-sitter: use tree-sitter-kotlin.

## Input Generation (`inputs/gen_inputs.rb`)

Internal benchmarks only. Deterministic (seeded PRNG). Each grammar × 3 sizes × 2–3 variants.

Comparison benchmarks must not use generated inputs. They use grammar/input files from https://github.com/arithy/packcc/tree/main/benchmark verbatim.

### Sizes
Input size array: `[1k, 10k, 1m]`

| Label | Target | Purpose |
|-------|--------|---------|
| `xs`  | 1 KB   | Baseline, startup cost |
| `sm`  | 10 KB  | Small file |
| `lg`  | 1 MB   | Throughput stress |

### Variants per grammar

**calc**: `flat` (long chain: `1+2+3+...`), `deep` (nested parens: `((((1+2)+3)+4)...)`), `mixed`

**json**: `flat` (large array of numbers), `deep` (nested objects 100+ levels), `wide` (object with many keys), `mixed` (realistic structure)

**kotlin**: single large file (from packcc repo input, plus generated padding if needed for larger sizes)

## Two Benchmark Modes

### 1. Internal Benchmarks (`benchmark/internal/`)

Nest-only. Compare own options, collect detailed metrics.

**Matrix:**
| Dimension | Values |
|-----------|--------|
| Memoize   | `none`, `naive`, `shared` |
| Clang opt | calc: `-O0`, `-O2`; json/kotlin: `-O2` only |
| Grammar   | calc, json, kotlin |
| Input     | all sizes × all variants |

= 3 memoize × ((2 opt × calc inputs) + (1 opt × json inputs) + (1 opt × kotlin inputs))

**Metrics collected per run:**

| Metric | How |
|--------|-----|
| Parse wall time (µs) | `clock_gettime(CLOCK_MONOTONIC)`, median of N iterations |
| Throughput (MB/s) | input_bytes / parse_time |
| Peak RSS (KB) | `/usr/bin/time -l` → `maximum resident set size` (bytes on macOS, divide by 1024) |
| Token count | `darray_size(tc->tokens)` on root chunk |
| Chunk count | `darray_size(tt->table)` (= number of scopes opened) |
| Max scope depth | Track during tree walk |
| Memoize table total bytes | `sizeof_col * token_count` summed across chunks |
| Lex time vs parse time | Instrument: time VPA stage and PEG stage separately |

**Output:** CSV per grammar:
```
grammar,input,size_bytes,memoize,opt,parse_us,throughput_mbs,rss_kb,token_count,chunk_count,max_depth,memo_table_bytes,lex_us,peg_us
```

**Key questions answered:**
1. Does `shared` memoize beat `naive`? When?
2. How does memoize table size scale with token count?
3. What fraction of time is lex (VPA) vs parse (PEG)?
4. `-O2` vs `-O0` improvement on generated LLVM IR for internal calc case?
5. Deep vs flat vs wide — which stresses what?

### 2. Comparison Benchmarks (`benchmark/`)

Nest (best config: `shared -O2`) vs PackCC vs tree-sitter.

Use grammar/input from https://github.com/arithy/packcc/tree/main/benchmark for cross-framework comparison. No generated inputs here. Same machine, same measurement.

**Metrics (cross-framework comparable):**

| Metric | Unit |
|--------|------|
| Parse throughput | MB/s |
| Peak RSS | KB |
| Grammar compile time | ms (grammar → parser source) |
| Parser build time | ms (parser source → binary) |
| Binary size | bytes |

**Output:** CSV:
```
framework,grammar,input,size_bytes,parse_us,throughput_mbs,rss_kb,compile_ms,build_ms,binary_bytes
```

## Runner Design

### nest_runner.c.erb

ERB template. For each grammar, `nest c` generates `<prefix>.h` + `<prefix>.ll`.
Runner links against them.

```c
#include "<%= prefix %>.h"
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// No getrusage needed — orchestrator wraps with /usr/bin/time -l

int main(int argc, char** argv) {
    // argv[1] = input file, argv[2] = iterations
    // read file into buffer
    // create UStr
    // warm up: 3 iterations
    // timed: N iterations, collect each time
    // report: median, min, max, token stats
    // CSV line to stdout
}
```

### packcc_runner.c

Wrapper already embedded in each `.peg` file's `%%` section. Just needs timing instrumentation around `{prefix}_parse()`.

### treesitter_runner.c

```c
#include <tree_sitter/api.h>
// link against grammar's parser.c + libtree-sitter from brew
// ts_parser_new() → ts_parser_set_language() → ts_parser_parse_string()
// time the ts_parser_parse_string() call
```

## setup.rb

All builds use `xcrun clang`. No cross-compilation.

Tasks:
1. Clone packcc → `benchmark/vendor/packcc/`, build `packcc` binary with `xcrun clang -O2`
2. `brew install tree-sitter` (skip if already installed)
3. Clone tree-sitter-json, tree-sitter-kotlin (`https://github.com/tree-sitter-grammars/tree-sitter-kotlin`) → `benchmark/vendor/`
4. Copy PackCC benchmark grammars/inputs to `benchmark/inputs/compare/` (for example `calc.txt`, `json.json`, `kotlin.kt`) and keep them verbatim for comparison runs
5. Copy upstream licenses for reused benchmark assets/code into `benchmark/licenses/`

## run.rb

Main orchestrator. Subcommands:

```
ruby benchmark/run.rb setup          # run setup.rb
ruby benchmark/run.rb gen            # generate internal-only inputs
ruby benchmark/run.rb internal       # run internal benchmarks
ruby benchmark/run.rb compare        # run comparison benchmarks
ruby benchmark/run.rb report         # generate markdown report from CSVs
ruby benchmark/run.rb all            # gen(internal only) + internal + compare + report
```

Each `run` step:
1. Build parsers (nest: `nest c` + `clang`, packcc: `packcc` + `cc`, tree-sitter: `tree-sitter generate` + `cc`)
2. Run each (input, config) pair N times
3. Collect timing via runner's CSV stdout
4. Aggregate into results/ CSVs

**Iteration count:** adaptive. Target ≥1s total per measurement. Min 5, max 1000.

## Report Format

Markdown tables + summary.

### Internal Report

Generate 2 * input_sizes internal tables:
1. calc-only optimization tables for all input sizes
   - row header=memoize_mode
   - col header=`-O0` and `-O2` side-by-side with metrics [MB/s, RSS MB, Tokens, Chunks]
2. all-grammar `-O2` tables
   - same structure as calc-only table

### Comparison Report Example:
```
## json — 100KB mixed input

| Framework    | Parse µs | MB/s  | RSS KB | Compile ms | Binary KB |
|--------------|----------|-------|--------|------------|-----------|
| nest         | 980      | 102.0 | 2560   | 45         | 32        |
| packcc       | 1200     | 83.3  | 1800   | 12         | 28        |
| tree-sitter  | 1500     | 66.7  | 4096   | 200        | 180       |
```

## Implementation Order

Each file must be completed fully before moving to the next. No scaffolding stubs, no placeholders, no "TODO" passes. A file is either not started or done.

1. `benchmark/grammars/calc/grammar.nest` — complete nest calc grammar
2. `benchmark/grammars/json/grammar.nest` — complete nest json grammar
3. `benchmark/grammars/calc/grammar.peg` — copy from packcc repo
4. `benchmark/grammars/json/grammar.peg` — copy from packcc repo
5. `benchmark/grammars/calc/grammar.js` — tree-sitter calc grammar
6. `benchmark/grammars/json/grammar.js` — tree-sitter json grammar (from tree-sitter-json)
7. `benchmark/inputs/gen_inputs.rb` — full input generator for internal benchmarks / ablation studies only, working end-to-end
8. `benchmark/setup.rb` — install packcc + tree-sitter + grammar repos into vendor/, fully working
9. `benchmark/runners/nest_runner.c.erb` — complete timing harness, CSV output, all metrics
10. `benchmark/runners/packcc_runner.c` — complete timing harness, CSV output
11. `benchmark/runners/treesitter_runner.c` — complete timing harness, CSV output
12. `benchmark/internal/run_internal.rb` — full internal benchmark: build + run nest across memoize × opt matrix (`-O0` only for calc), CSV output + markdown report with calc `-O0/-O2` table plus all-grammar `-O2` table
13. `benchmark/run.rb` — full orchestrator: setup/gen/internal/compare/report/all subcommands, complete
14. `benchmark/grammars/kotlin/grammar.nest` — translate kotlin.peg to .nest, complete
15. `benchmark/grammars/kotlin/grammar.peg` — copy from packcc repo
16. `benchmark/grammars/kotlin/grammar.js` — tree-sitter-kotlin grammar
17. `benchmark/licenses/*` — upstream licenses for reused PackCC/tree-sitter benchmark assets
18. `benchmark/benchmark` — one-shot shell script entry point, calls `ruby benchmark/run.rb all`
