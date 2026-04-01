Simple LLVM IR writer

Features:
- For convenient text IR building -- text target so building does not depend on LLVM project
- simple, just write to `FILE*`
- auto numbering registers
- auto numbering labels
- emits debug information
- can create debugtrap

Basic Creation:
- `irwriter_new(FILE*, char* target_triple)`
- `irwriter_del()`

Module prelude and epilogue
- `irwriter_start()`, 
- `irwriter_end()`

Function prelude and epilogue
- `irwriter_define_start()`
  - also with file_path, compiling cwd for debug info
- `irwriter_define_end()`

### Book-keeping registers and labels

Internal structure

```c
struct IrWriter {
  FILE* out;
  char* imms;
  int32_t max_reg;
  int32_t max_label;
};
```

`imms` is a sequence of immediate value strings, segmented by `'\0'`.

when we track an `imm`, we append the string to `imms`, return the negative value of the offset of the imm string in the buf.

Value token: universal representation for imm & regs so ops won't need to distinguish between immediate / register values:

```c
// in irwriter.h
typedef int32_t IrVal

// using
IrVal lhs = irwriter_reg(w); // alloc a new register to use, positive integer
IrVal rhs = irwriter_imm(w, "24"); // returns negative integer, which is the -index offset in `imms`
IrVal res = irwriter_binop(w, op, ty, lhs, rhs);
```

Labeling:

```c
int32_t my_label = irwriter_label(w); // creates a label
irwriter_bb_at(w, my_label);
```

### Essential API

We target DFA generation, so there are no loops and no need for a complex Dominance-Frontier algorithm.

- binop, icmp
- insertvalue: insert a scalar into an aggregate at a given index
  - `insertvalue(agg_ty, agg_val, elem_ty, elem_val, idx)` -- string element value
  - used to build `{i32, i32}` return pairs: first insert at index 0 from undef, then insert at index 1

### ABI widening

When `ret_type` is `{i32, i32}`, the external signature is widened to `{i64, i64}` so C callers
can use a `struct { int64_t; int64_t; }` directly. Parameters declared as `i32` are widened to
`i64` in the signature with `trunc` instructions at function entry. Returns are widened via
`extractvalue` + `sext` + `insertvalue {i64, i64}` before `ret`. Internal IR stays `i32`.

### Security

- check file_name should not contain `"` or `\\`
- check function_name should not contain `"` or `\\`
- check target_triple: must be non-empty, only `[a-zA-Z0-9._-]`
- check directory should not contain `"` or `\\`
- abort on violation

### Avoid allocations, avoid re-buffering

For example, we'd rather use 2 branches like the following to avoid a local `char[16]` then `snprintf`:

```c
if (v < 0) {
  fprintf(out, "... %s ...", imms - v);
} else {
  fprintf(out, "... %%%r ...", v);
}
```
