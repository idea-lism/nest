The PEG IR helpers in src/peg_ir.c wrap irwriter for multi-instruction sequences.

# API

```c
typedef struct {
  IrWriter* ir_writer;
  IrVal tokens;     // TokenChunk->tokens register, so we can find the token id
  IrVal col_index;  // to match: tokens[col_index].term_id
  IrVal fail_label; // label for failure path
  IrVal stack;      // register for backtrack stack
  IrVal stack_bp;   // base-pointer for stack calls
  IrVal ret_register;
  // and other context vars if needed in implementation

  const char* scope_name;
  Symtab scoped_rule_names;
} PegIrCtx;
```

- `peg_ir_term(ctx, int32_t term_id)`: emit code to match a terminal
- `peg_ir_call(ctx, int32_t scoped_rule_id)`: emit code to call a rule
- `peg_ir_fast_call(ctx, int32_t scoped_rule_id)`: emit code to go to a rule, which doesn't require stack manipulation
- `peg_ir_seq(ctx, int32_t* seq)`: generate matcher IR for the seq
- `peg_ir_choice(ctx, int32_t* branches)`: generate matcher IR for the broken down rule
- `peg_ir_maybe(ctx, scoped_rule_id)`
- `peg_ir_star(ctx, lhs_scoped_rule_id, rhs_scoped_rule_id)`
- `peg_ir_plus(ctx, lhs_scoped_rule_id, rhs_scoped_rule_id)`
- `peg_ir_emit_helpers(irwriter)`: define internal functions `@table_gep` (to access memoize table's slot), `@save`, `@restore`, and `@bit_test`, `@bit_deny`, `@bit_exclude` as defined in [PEG](peg.md)

# Stack data layout and pseudo sub-rule calling

Layout

```c
typedef union {
  int64_t col;
  void* ret_site;
} StackSlot;

StackSlot* stack;
```

Operations

```c
// @save
  stack++;
  stack->col = col;

// @restore
  col = stack->col;
  stack--;

// discard
  stack--;

// call sub-rule
  stack++;
  stack->ret_site = &&ret_label;
  br {scope_name}${scoped_rule_name};
ret_label:
  parsed = %ret;

// call return
{scope_name}${scoped_rule_name}:
  %ret_addr = stack->ret_size;
  stack--;
  %bp = stack
  %ret = ...
  stack = %bp;
  br %ret_addr;

// fast_call sub-rule
  %fast_ret_addr = &&ret_label;
  br {scope_name}${scoped_rule_name};
ret_label:
  parsed = %ret

// fast_call return
{scope_name}${scoped_rule_name}:
  %ret = ...
  br %%fast_ret_addr
```

# Code gen

`fail` means passing the on-failure label.

### Sequence

```
gen(a b, col, fail):
  r1 = gen(a, col, fail)
  r2 = gen(b, col + r1, fail)
  %ret = r1 + r2
```

### Ordered choice

Refer to the branches syntax in [parse.md](parse.md).

```
gen(a / b, col, fail):
  save(col)
  r = gen(a, col, alt_bb)
  discard()
  br(done_bb)
alt_bb:
  restore()
  discard()
  r2 = gen(b, col, fail)
  discard()
  br(done_bb)
done_bb:
  %ret = phi(r from succ_bb, r2 from alt_bb)
```

For N-ary ordered choice `a / b / c / ...`, the pattern generalizes: one `save` at the start, `discard` after each successful match, `restore` + `discard` + `save` at each non-last retry, `restore` + `discard` before the last attempt.

### Optional (?)

Always succeeds, never branches to fail.

```
gen(e?, col, fail):
  r = gen(e, col, miss_bb)
  br(done_bb)
miss_bb:
  br(done_bb)
done_bb:
  %ret = phi(r from try_bb, 0 from miss_bb)
```

### One-or-more (+), possessive

First match must succeed. Then loop greedily, no backtracking.

```
gen(e+, col, fail):
  first = gen(e, col, fail)
  br(loop_bb)
loop_bb:
  acc = phi(first from entry_bb, next from body_bb)
  r = gen(e, col + acc, end_bb)
  next = acc + r
  br(loop_bb)
end_bb:
  %ret = acc
```

### Zero-or-more (*), possessive

Same loop as `+`, but starts with zero matches (always succeeds).

```
gen(e*, col, fail):
  br(loop_bb)
loop_bb:
  acc = phi(0 from entry_bb, next from body_bb)
  r = gen(e, col + acc, end_bb)
  next = acc + r
  br(loop_bb)
end_bb:
  %ret = acc
```

### One-or-more with interlace (+\<sep\>)

Matches `e (sep e)*`. First element required, then alternating separator and element.

```
gen(e+<sep>, col, fail):
  first = gen(e, col, fail)
  br(loop_bb)
loop_bb:
  acc = phi(first from entry_bb, next from body_bb)
  sr = gen(sep, col + acc, end_bb)
  er = gen(e, col + acc + sr, end_bb)
  next = acc + sr + er
  br(loop_bb)
end_bb:
  %ret = acc
```

### Zero-or-more with interlace (*\<sep\>)

Matches `(e (sep e)*)?`. Zero matches is OK.

```
gen(e*<sep>, col, fail):
  first = gen(e, col, empty_bb)
  br(loop_bb)
loop_bb:
  acc = phi(first from entry_bb, next from body_bb)
  sr = gen(sep, col + acc, end_bb)
  er = gen(e, col + acc + sr, end_bb)
  next = acc + sr + er
  br(loop_bb)
empty_bb:
  br(end_bb)
end_bb:
  result = phi(acc from loop_bb, 0 from empty_bb)
  %ret = result
```

# Notes

- `gen` always has an `fail` label. On failure, control transfers there. On success, it falls through and returns the match length.
- `?` and `*` always succeed — they never branch to `fail`.
- `+` and `*` are possessive (no backtracking).
- `+<sep>` / `*<sep>` interlace: the separator is only consumed when followed by a successful element match. The loop exits before accumulating the separator, discarding both `sr` and `er`.
