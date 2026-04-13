The PEG IR helpers in src/peg_ir.c wrap irwriter for multi-instruction sequences.

Handles parsing that's UNRELATED to memoize table access.

# API

```c
// define shared registers for IR generation
typedef struct {
  IrWriter* ir_writer;

  // some scoped rule's attributes that is required at gen time
  int64_t tag_bit_offset;

  // shared registers, they won't be re-assigned
  IrVal tc; // TokenChunk %tc
  IrVal tokens; // %tc->tokens

  // shared allocas
  IrVal col; // current %col
  IrVal stack_ptr; // register storing the input stack_ptr
  IrVal stack_bp;  // base-pointer for stack calls
  IrVal parse_result; // PegRef %parse_result
  IrVal fast_ret_addr; // a shared register for fast-return
  IrVal tag_bits; // %tag_bits for successful match to set
  IrVal parsed_tokens; // %parsed_tokens
  // and other context vars if needed in implementation

} PegIrCtx;
```

- `peg_ir_emit_parse(ctx, ScopedUnit* unit, IrVal fail_label)`: emit IR for ScopedUnit tree (see definition in [PEG spec](peg.md#scope-closures)).
  - by unit's kind, dispatch to different `gen` parts as described below
  - at the success end, set `parsed_tokens` register
  - at the success end, update col register with `col += parsed_tokens`
  - if unit.tag_offset >= 0, also `%tag_bits |= {1 << (unit.tag_offset + unit.tag_bit_offset)}`
- `peg_ir_emit_ret(ctx)`
  - at the end, impl call return, which updates stack (see below)
- `peg_ir_emit_helpers(irwriter)`: define internal functions `@save`, `@restore`, and `@bit_test`, `@bit_deny`, `@bit_exclude` as defined in [PEG](peg.md)
  - `void @save(ptr %stack_ptr, ptr %col)`
  - `void @restore(ptr %stack_ptr, ptr %col)`

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

// discard
  stack--;

// call sub-rule
  stack++;
  stack->ret_site = &&ret_label;
  br {scoped_rule_name};
ret_label:
  parsed = %ret;

// call return (generated at the end of peg_ir_emit_ret)
{scoped_rule_name}:
  %ret_addr = stack->ret_site;
  stack--;
  %bp = stack
  %ret = ...
  stack = %bp;
  indirectbr %ret_addr;
```

# Code gen

`fail` means passing the on-failure label.

`gen(unit, fail_label)` means `peg_ir_emit_parse(ctx, unit, fail_label)`

Call & term are trivial so we don't specify here.

### Sequence

```c
gen(seq(a, b), fail):
  save(col)
  r1 = gen(a, fail_bb)
  r2 = gen(b, fail_bb)
  %ret = r1 + r2
  br(done_bb)
fail_bb:
  restore()
  discard()
  br(fail)
done_bb:
```

### Ordered choice

Refer to the branches syntax in [parse.md](parse.md).

```c
gen(branches(a, b, ...), fail):
alt1:
  save(col)
  r1 = gen(a, alt2)
  discard()
  br(done_bb)
alt2:
  col = restore()
  r2 = gen(b, alt2)
  discard()
  br(done_bb)
...
altn:
  col = restore()
  discard()
  rn = gen(b, fail)
  br(done_bb)
done_bb:
  %ret = phi(r1 from alt1, r2 from alt2, ... rn from altn)
```

### Optional (?)

Always succeeds, never branches to fail. No need backtracking.

```c
gen(e?, fail):
  r = gen(e, miss_bb)
  br(done_bb)
miss_bb:
  br(done_bb)
done_bb:
  %ret = phi(r from try_bb, 0 from miss_bb)
```

### One-or-more (+), possessive

First match must succeed. Then loop greedily, no need backtracking.

```c
gen(e+, fail):
  first = gen(e, fail)
  br(loop_bb)
loop_bb:
  acc = phi(first from entry_bb, next from body_bb)
  r = gen(e, end_bb)
  next = acc + r
  br(loop_bb)
end_bb:
  %ret = acc
```

### Zero-or-more (*), possessive

Same loop as `+`, but starts with zero matches (always succeeds). no need backtracking.

```c
gen(e*, fail):
  br(loop_bb)
loop_bb:
  acc = phi(0 from entry_bb, next from body_bb)
  r = gen(e, end_bb)
  next = acc + r
  br(loop_bb)
end_bb:
  %ret = acc
```

### One-or-more with interlace (+\<sep\>)

Matches `e (sep e)*`. First element required, then alternating separator and element.

```c
gen(e+<sep>, fail):
  first = gen(e, fail)
  br(loop_bb)
loop_bb:
  acc = phi(first from entry_bb, next from body_bb)
  save(col)
  sr = gen(sep, sep_fail)
  er = gen(e, sep_fail)
  discard()
  next = acc + sr + er
  br(loop_bb)
sep_fail:
  restore()
  discard()
  br(end_bb)
end_bb:
  %ret = acc
```

### Zero-or-more with interlace (*\<sep\>)

Matches `(e (sep e)*)?`. Zero matches is OK.

```c
gen(e*<sep>, fail):
  first = gen(e, empty_bb)
  br(loop_bb)
loop_bb:
  acc = phi(first from entry_bb, next from body_bb)
  save(col)
  sr = gen(sep, sep_fail)
  er = gen(e, sep_fail)
  discard()
  next = acc + sr + er
  br(loop_bb)
sep_fail:
  restore()
  discard()
  br(end_bb)
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

# Acceptance criteria

- If spec already given a variable/function name, don't re-invent yet another name for the same idea.
- Helper functions are real `define internal` helper functions in LLVM IR.
- No fabricated extra features / conditionals / checks / vars / functions that's not in spec -- if need, ask first.
- For shared context registers / allocas, the names should be readable.
