The PEG IR helpers in src/peg_ir.c wrap irwriter for multi-instruction sequences.

Handles parsing that's mostly unrelated to memoize table access except for the multiplier units.

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
  IrVal parse_result; // PegRef %parse_result
  IrVal tag_bits; // %tag_bits for successful match to set
  // and other context vars if needed in implementation

} PegIrCtx;
```

- `peg_ir_emit_parse(ctx, ScopedUnit* unit, IrVal fail_label)`: emit IR for ScopedUnit tree (see definition in [PEG analyze](peg_analyze.md#scope-closures)).
  - by unit's kind, dispatch to different `gen` parts as described below
  - if unit.tag_bit_local_offset >= 0, also `%tag_bits |= {1 << (unit.tag_bit_local_offset + ctx.tag_bit_offset)}`
- `peg_ir_emit_call(ctx, name)` pushes ret_site, then col (details see below)
- `peg_ir_emit_ret(ctx)` pops col, back to ret_site
  - at the end, impl call return, which updates stack (see below)
- `peg_ir_emit_helpers(irwriter)`: define internal functions `@save`, `@restore`
  - `void @save(ptr %stack_ptr, ptr %col)`
  - `void @restore(ptr %stack_ptr, ptr %col)`
  - `i64 @top(ptr %stack_ptr)`: get the top stored col (but not update `%col`)
- `peg_ir_emit_bit_helpers(irwriter)`: emit shared-mode bit helper definitions: `@bit_test`, `@bit_deny`, `@bit_exclude` as defined in [PEG Gen](peg_gen.md)
- `peg_ir_emit_gep_helpers(irwriter)`: emit GEP and tag writeback helper definitions
  - `ptr @gep_slot(ptr %table, i64 %col, i64 %sizeof_col, i64 %slot_byte_offset)`: compute pointer to memoize slot
  - `ptr @gep_tag(ptr %table, i64 %col, i64 %sizeof_col, i64 %tag_byte_offset)`: compute pointer to tag bits bucket
  - `void @tag_writeback(ptr %table, i64 %col, i64 %sizeof_col, i64 %tag_byte_offset, i64 %clear_mask, i64 %tag_bits)`: load old tag bits, clear with mask, OR in new tag_bits, store back

One optimization idea is chained-slot-writes for multiplier matchings: if `a*` matches `a a a`, we can cache `a*=3, a*=2, a*=1` on all three positions. but that would need the IR book-keeping all parsed sizes and calculate accumulatives, which is too complex. So we keep things simple, memoize the whole parsed size as other rules.

# Stack data layout and pseudo sub-rule calling

Layout

```c
typedef union {
  int64_t col;
  uint64_t tag_bits; // when calling sub-rule we also need to push tag_bits
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
  { if caller has tag_bits }
    stack++;
    stack->tag_bits = tag_bits;
  { end if }
  stack++;
  stack->ret_site = &&ret_label;
  stack++;
  stack->col = col;
  tag_bits = 0;
  br {scoped_rule_name};
ret_label:
  { if caller has tag_bits }
    tag_bits = stack->tag_bits;
    stack--;
  { end if }
  parsed = %ret;

// call return (generated at the end of peg_ir_emit_ret)
{scoped_rule_name}:
  stack--;
  %ret_addr = stack->ret_site;
  stack--;
  %ret = ...
  indirectbr %ret_addr, [caller_ret_labels...];
```

The `indirectbr` destination list must include return labels from **all** call sites that target this rule. Labels follow the naming convention `callsite$${caller_id}$${site}`, derived from the callee's `call_sites` darray (see [peg_analyze](peg_analyze.md#call-site-analysis)).

- `peg_ir_emit_call`: the caller tracks a local site counter. Emits `blockaddress` and return label as `callsite$${caller_id}$${n}`.
- `peg_ir_emit_ret`: iterates the current rule's `call_sites` darray to emit the complete `indirectbr` label list.

# Code gen

`fail` means passing the on-failure label.

`gen(unit, fail_label)` means `peg_ir_emit_parse(ctx, unit, fail_label)`.

Term increments `col` by 1.

Call & term are trivial so we don't specify here.

### Sequence

```c
gen(seq(a, b), fail):
  save(col)
  gen(a, fail_bb)
  gen(b, fail_bb)
  br(done_bb)
fail_bb:
  restore()
  discard()
  br(fail)
done_bb:
  discard()
```

### Ordered choice

Refer to the branches syntax in [parse.md](parse.md).

```c
gen(branches(a, b, ...), fail):
alt1:
  save(col)
  gen(a, alt2)
  discard()
  br(done_bb)
alt2:
  col = restore()
  gen(b, alt2)
  discard()
  br(done_bb)
...
altn:
  col = restore()
  discard()
  gen(b, fail)
  br(done_bb)
done_bb:
```

### Optional (?)

Always succeeds, never branches to fail. No need backtracking.

```c
gen(e?, fail):
  gen(e, done_bb)
  br(done_bb)
done_bb:
```

### One-or-more (+), possessive

First match must succeed. Then loop greedily, no need backtracking.

```c
gen(e+, fail):
  gen(e, fail)
  br(loop_bb)
loop_bb:
  gen(e, end_bb)
  // if e is nullable, check advancement to prevent infinite loop
  br(loop_bb)
end_bb:
```

### Zero-or-more (*), possessive

Same loop as `+`, but starts with zero matches (always succeeds). no need backtracking.

```c
gen(e*, fail):
loop_bb:
  gen(e, end_bb)
  // if e is nullable, check advancement to prevent infinite loop
  br(loop_bb)
end_bb:
```

### One-or-more with interlace (+\<sep\>)

Matches `e (sep e)*`. First element required, then alternating separator and element.

```c
gen(e+<sep>, fail):
  gen(e, fail)
  br(loop_bb)
loop_bb:
  save(col)
  sr = gen(sep, sep_fail)
  er = gen(e, sep_fail)
  discard()
  // if e is nullable, check advancement to prevent infinite loop
  br(loop_bb)
sep_fail:
  restore()
  discard()
  br(end_bb)
end_bb:
```

### Zero-or-more with interlace (*\<sep\>)

Matches `(e (sep e)*)?`. Zero matches is OK.

```c
gen(e*<sep>, fail):
  gen(e, end_bb)
  br(loop_bb)
loop_bb:
  save(col)
  gen(sep, sep_fail)
  gen(e, sep_fail)
  discard()
  // if e is nullable, check advancement to prevent infinite loop
  br(loop_bb)
sep_fail:
  restore()
  discard()
  br(end_bb)
end_bb:
```

# Notes

- `gen` always has an `fail` label. On failure, control transfers there. On success, it falls through and returns the match length.
- `?` and `*` always succeed — they never branch to `fail`.
- `+` and `*` are possessive (no backtracking).
- `+<sep>` / `*<sep>` interlace: the separator is only consumed when followed by a successful element match. The loop exits before accumulating the separator, discarding both `sr` and `er`.

# Stack depth tracking in PegIrCtx (compiler-side assertion)

 Add int32_t stack_depth to PegIrCtx. Instrument every stack operation in peg_ir.c:

 - _emit_call_save → ctx->stack_depth++
 - _emit_call_restore → ctx->stack_depth--
 - _emit_discard → ctx->stack_depth--
 - peg_ir_emit_call → ctx->stack_depth += 2 (or 3 with tags)
 - Return-site in _emit_call → corresponding decrements

 At every fail_label branch and done_bb branch, assert(ctx->stack_depth == entry_depth). This catches imbalanced codegen at compile time (when nest runs), not at parse time.

# Acceptance criteria

- If spec already given a variable/function name, don't re-invent yet another name for the same idea.
- Helper functions are real `define internal` helper functions in LLVM IR.
- No fabricated extra features / conditionals / checks / vars / functions that's not in spec -- if need, ask first.
- For shared context registers / allocas, the names should be readable.

# Tests

In `test_peg_ir.c`, Create irwriter, emit:

- `peg_ir_emit_helpers`
- `peg_ir_emit_gep_helpers`

And expose them as `define` instead of `define internal`, so we can test all generated helper functions.
