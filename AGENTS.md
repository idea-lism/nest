### Architecture

1. A regex-to-LLVM-IR compiler.
   - Regex patterns → NFA → DFA (minimized) → LLVM IR text → native code via clang.
2. A optimized PEG parser.
   - Graph-coloring optimized parsing. uses kissat

IMPLEMENTATION MUST FOLLOW SPECS.

### Project Structure

src/
  nest.c: CLI entry point. subcommands: `nest l`, `nest c`, `nest h`, `nest r`. follows specs/cli.md
  re.h: regex builder on top of automata — intervals, ranges, NFA construction. follows specs/re.md
  re.c: impls re.h
  re_ir.h: intermediate representation for regex patterns (opcodes like RE_IR_APPEND_CH, RE_IR_FORK, etc.). follows specs/re_ir.md
  re_ir.c: impls re_ir.h, converts ReIr opcodes into re.h calls
  aut.h: automata constructing (NFA→DFA), minimization, LLVM IR generation. follows specs/aut.md
  aut.c: impls aut.h
  irwriter.h: LLVM IR text emitter (functions, basic blocks, instructions, debug metadata). follows specs/irwriter.md
  irwriter.c: impls irwriter.h
  parse.h: parser for `.nest` source files, owns ParseState and error reporting. follows specs/parse.md
  parse.c: impls parse.h, lexes and parses `.nest` syntax into token tree
  parse_gen.c: build-time tool that generates lexer functions for `.nest` syntax. follows specs/parse_gen.md
  post_process.h: ordered compile passes after parsing (macro inlining, tag generation, validation). follows specs/post_process.md
  post_process.c: impls post_process.h
  peg.h: PEG types shared across analyze/gen/ir (PegUnit, PegRule, PegAnalysis). follows specs/peg_analyze.md, specs/peg_gen.md
  peg_analyze.c: PEG grammar analysis — FIRST/FOLLOW sets, conflict detection, memoization planning. follows specs/peg_analyze.md
  peg_gen.c: PEG code generation — emits C header with parser tables and structure definitions. follows specs/peg_gen.md
  peg_ir.c: PEG IR emission — generates LLVM IR for PEG parsing functions. follows specs/peg_ir.md
  peg_ir.h: PEG IR context (PegIrCtx) shared state for IR emission. follows specs/peg_ir.md
  vpa.h: visibly pushdown automaton — scoped lexer with hooks, regex compilation per scope. follows specs/vpa.md
  vpa.c: impls vpa.h
  coloring.h: graph k-coloring via kissat SAT solver (falls back to DSatur on Windows). follows specs/coloring.md
  coloring.c: impls coloring.h
  graph.h: adjacency-list graph — edges, random generation, max clique finding
  graph.c: impls graph.h
  header_writer.h: C header file emitter (structs, enums, defines, typedefs)
  header_writer.c: impls header_writer.h
  token_tree.h: hierarchical token storage — chunks, scopes, memoization tables. follows specs/token_tree.md
  token_tree.c: impls token_tree.h
  symtab.h: string interning table — intern/find/get by integer ID. follows specs/symtab.md
  symtab.c: impls symtab.h
  ustr.h: UTF-8 string with codepoint-level operations (size, slice, iterate). follows specs/ustr.md
  ustr.c: impls ustr.h, dispatches to platform-specific backends
  ustr_neon.c: ARM NEON accelerated UTF-8 validation
  ustr_avx.c: x86 AVX2 accelerated UTF-8 validation
  ustr_naive.c: portable fallback UTF-8 validation
  ustr_intern.h: internal interface shared by ustr backends
  bitset.h: dynamic bitset with set operations (or, and, equal). follows specs/bitset.md
  bitset.c: impls bitset.h
  darray.h: generic dynamic array (fat pointer, type-erased). follows specs/darray.md
  darray.c: impls darray.h
test/
  test_re.c, test_re_ir.c: regex builder and IR tests
  test_aut.c: automata NFA→DFA and LLVM IR generation tests
  test_irwriter.c: IR text emitter tests
  test_parse.c, test_parse_gen.c: parser and lexer generator tests
  test_peg_analyze.c, test_peg_gen.c, test_peg_ir.c: PEG subsystem tests
  test_post_process.c: post-processing pass tests
  test_vpa.c: VPA lexer tests
  test_coloring.c: graph coloring tests
  test_symtab.c: symbol table tests
  test_token_tree.c: token tree tests
  test_ustr.c, test_bitset.c: utility tests
  bench_ustr.c: UTF-8 string benchmarks
  compat.c: platform compatibility helpers for tests
test/examples/: example `.nest` files (calc, clike, interp, json, minilang, words). follows specs/example_test.md
scripts/
  test: run all tests (accepts `debug` or `release` arg)
  bench: run benchmarks
  coverage: build & generate coverage report
  test_examples: run example `.nest` files
  amalgamate.rb: produce single-file amalgamation
  gen_str_header.rb: generate string constant headers
config.rb: platform-related config, generates build.ninja
config.in.rb: defined sources, targets, dependencies
specs/: design docs; read the relevant one before editing a subsystem

### Common Tasks

```sh
ruby config.rb debug
ninja
ninja format
```

Run tests / benchmarks:
```sh
scripts/test         # in release mode
scripts/test debug
scripts/benchmark
scripts/test_examples
```

Single test flow:
```sh
ruby config.rb debug
ninja build/debug/test_aut
build/debug/test_aut
```

Adding new source files:
- add the entry in config.in.rb
- add related test

### Utils to Avoid Boilerplates

Ustr
```c
// UTF-8 string with codepoint-level indexing
char* s = ustr_new(strlen(data), data);  // create from raw bytes
char* s2 = ustr_from_file(fp);           // read entire file
int32_t len = ustr_size(s);              // codepoint count
int32_t bytes = ustr_bytesize(s);        // byte count
int32_t cp = ustr_cp_at(s, 3);           // codepoint at index
char* sub = ustr_slice(s, 2, 5);         // slice by codepoint range
UstrByteSlice bs = ustr_slice_bytes(s, 2, 5); // {.s, .size} byte slice
UstrIter it; ustr_iter_init(&it, s, 0);  // iterator from codepoint offset
int32_t ch = ustr_iter_next(&it);        // next codepoint (-1 on end)
ustr_del(s);                             // free
```

Darray
```c
// generic dynamic array (fat pointer style)
int32_t* arr = darray_new(sizeof(int32_t), 0);
darray_push(arr, 42);
darray_push(arr, 99);
size_t n = darray_size(arr);   // 2
arr = darray_concat(arr, other);
// grow with extra capacity (logical size 10, capacity 16)
arr = darray_grow2(arr, 10, 16);
darray_del(arr);
```

IrWriter
```c
// LLVM IR text emitter
IrWriter* w = irwriter_new(out_file, target_triple);
irwriter_start(w, "source.ll", ".");
irwriter_define_startf(w, "match", "{i64, i64} @match(i64 %%state_i64, i64 %%cp_i64)");
irwriter_set_widen_ret(w);
irwriter_bb(w);
// ... emit instructions ...
irwriter_define_end(w);
irwriter_end(w);
irwriter_del(w);
```

HeaderWriter
```c
// C header file emitter with indentation management
HeaderWriter* hw = hdwriter_new(out_file);
hdwriter_puts(hw, "#pragma once\n\n");
hdwriter_printf(hw, "typedef struct %s", name);
hdwriter_begin(hw);                       // emits " {\n", increments indent
hdwriter_printf(hw, "int32_t %s;\n", field);
hdwriter_end(hw);                         // decrements indent, emits "}\n"
hdwriter_puts(hw, ";\n");
hdwriter_del(hw);
```

TokenTree
```c
// hierarchical token storage with scope stack
TokenTree* tree = tt_tree_new(ustr_source);
tt_push(tree, scope_id);                           // push scope
tt_add(tree, tok_id, cp_start, cp_size, -1);       // add token
TokenChunk* chunk = tt_pop(tree);                   // pop scope
tt_tree_del(tree, false);
```

### Code Style

- types use camel case: `Aut`, `IrWriter`, `ColoringResult`
- vars & functions use snake case: `header_path`, `aut_gen_dfa`
- use stdint. for example, `int32_t` instead of `int`
- static (private) functions should start with `_`, names be simple as possible (without module prefix)

### Naming

Naming is important for readable code, should clearly represents what it does, and honest to spec, no re-inventing new variable/function names when specs has already named them.

Bad code: `n_tags`, `n_tokens`

Good code: `tag_size`, `token_bytesize`, `tag_size_in_i32`, `tag_size_in_i64`
