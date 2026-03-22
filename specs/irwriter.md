Simple LLVM IR writer

- For convenient text IR building -- so building does not depend on LLVM project
- simple, just write to `FILE*`
- maps state name to basic block label
- auto numbering

We target DFA generation, so there are no loops and no need for a complex Dominance-Frontier algorithm.

API

- basic: `irwriter_new(FILE*, char* target_triple)`, `irwriter_del()`
- module prelude and epilogue `irwriter_start()`, `irwriter_end()`
- function prelude and epilogue `irwriter_define_start()`, `irwriter_define_end()`
  - also with file_path, compiling cwd
- starting a basic block
- binop, binop_imm, icmp, icmp_imm
- insertvalue, insertvalue_imm: insert a scalar into an aggregate at a given index
  - `insertvalue(agg_ty, agg_val, elem_ty, elem_val, idx)` -- string element value
  - `insertvalue_imm(agg_ty, agg_val, elem_ty, elem_val, idx)` -- immediate int element value
  - used to build `{i32, i32}` return pairs: first insert at index 0 from undef, then insert at index 1
- emits debug information
- can create debugtrap

ABI widening

When `ret_type` is `{i32, i32}`, the external signature is widened to `{i64, i64}` so C callers
can use a `struct { int64_t; int64_t; }` directly. Parameters declared as `i32` are widened to
`i64` in the signature with `trunc` instructions at function entry. Returns are widened via
`extractvalue` + `sext` + `insertvalue {i64, i64}` before `ret`. Internal IR stays `i32`.

Security

- check file_name should not contain `"` or `\\`
- check function_name should not contain `"` or `\\`
- check target_triple: must be non-empty, only `[a-zA-Z0-9._-]`
- check directory should not contain `"` or `\\`
- abort on violation
