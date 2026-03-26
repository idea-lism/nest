# Project: tailorbird

1. A regex-to-LLVM-IR compiler. 
   - Regex patterns → NFA → DFA (minimized) → LLVM IR text → native code via clang.
2. A optimized PEG parser.
   - Graph-coloring optimized parsing.

## Develop

```
ruby config.rb debug
ninja
```

## Run tests / benchmarks

```
scripts/test # in release mode
scripts/test debug
scripts/benchmark
scripts/coverage # generate coverage report at build/coverage/html/index.html
```

## Clang-format

- line width: `120`
- if style: `if (xxx) {\n ... \n} else {...`
- pointer arg style: `foo* foo`
- enforce brace on block bodies
- on macOS, use `xcrun clang-format` to format
- others use default (indent = 2 space)

## Code style

- types use camel case
- vars & functions use snake case
- use stdint. for example, `int32_t` instead of `int`
- static (private) functions should start with `_`, names be simple as possible (without module prefix)

## Modules (src/)

| Module | Files | Purpose |
|--------|-------|---------|
| ustr | ustr.c, ustr.h, ustr_intern.h, ustr_{neon,avx,naive}.c | Fat-pointer UTF-8 string with SIMD validation (NEON/AVX2/scalar) |
| bitset | bitset.c, bitset.h | Dynamic bitset for NFA→DFA subset construction |
| irwriter | irwriter.c, irwriter.h | Emits textual LLVM IR (SSA numbering, basic blocks, DWARF debug, ABI widening) |
| aut | aut.c, aut.h | Core NFA/DFA engine: subset construction, Hopcroft minimization, IR emission |
| re | re.c, re.h | Regex builder API on top of aut (groups, alternation, ranges) |
| lex | lex.c, lex.h | Regex syntax parser, used internally by vpa.c (not distributed) |
| parse_gen | parse_gen.c | Build-time tool: generates DFA lexers for .nest syntax (inlines lex helpers) |
| ulex | ulex.c | CLI tool wrapping lex: reads pattern file, emits .ll |
| coloring | coloring.c, coloring.h | SAT-based graph coloring for PEG parser optimization (uses kissat) |

Dependency chain: ustr → bitset → irwriter → aut → re → lex → ulex
PEG parser: coloring (kissat) → peg

## Key design points

- Generated function signature: `(i32 state, i32 codepoint) -> {i32 new_state, i32 action_id}` (widened to i64 for C ABI)
- Moore/Mealy semantics for actions. MIN-RULE resolves conflicting action IDs.
- Special codepoints: BOF = -1, EOF = -2
- Regex syntax: `[a-z]` `[^...]` `.` `\s \w \d \h` `\n \t` `\u{XXXX}` `| () ? + *` `\a` (BOF) `\z` (EOF)
- Error codes from lex_add: LEX_ERR_PAREN = -1, LEX_ERR_BRACKET = -2

## Graph coloring (PEG optimization)

The `coloring` module uses kissat (SAT solver) to optimize PEG parser memory layout via graph coloring.

### Build dependency

kissat is built automatically by `config.rb` via `scripts/build_kissat.rb` (downloads rel-4.0.4, builds to `build/kissat/build/libkissat.a`).

### SAT encoding

Given interference graph G (vertices = PEG rules, edges = non-exclusive rules) and k colors:
- Variables: `x[v,c]` = vertex v has color c (1 ≤ v ≤ n, 0 ≤ c < k)
- Each vertex gets exactly one color: `∨c x[v,c]` and `¬x[v,c1] ∨ ¬x[v,c2]` for c1 ≠ c2
- Adjacent vertices get different colors: `¬x[u,c] ∨ ¬x[v,c]` for edge (u,v)
- Symmetry breaking: fix vertex 0 to color 0

### Segment layout

After coloring, vertices with same color form groups. Groups >32 elements split into 32-element segments.
Each vertex gets `(sg_id, seg_mask)` for bitset-based cache lookup in generated parser (see specs/peg.md).

## Specs

Detailed module specs live in `specs/*.md`. Refer to them when making changes to a module.

## Build outputs

- `out/libre.a` -- combined static lib (ustr + bitset + irwriter + aut + re)
- `out/re_rt.h` -- amalgamated single-header runtime (ustr + bitset)
- `build/<mode>/ulex` -- CLI tool
- `build/<mode>/parse_gen` -- build-time lexer generator for .nest syntax
- `build/<mode>/test_*` -- test binaries
