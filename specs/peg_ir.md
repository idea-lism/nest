The PEG IR helpers in src/peg_ir.c wrap irwriter for multi-instruction sequences.

Single LLVM instructions (add, icmp, select, phi, br) are emitted directly via irwriter.

# Low level ops

All values are `i32`. Convention: negative return = failure.

| Operation                              | LLVM IR expansion | Semantics |
|----------------------------------------|-------------------|-----------|
| `define internal @tok(token_id, col)`  | `%token_id == %col` | Match token at `col`. Returns match length (≥0) or negative on failure. |
| `define internal @call(rule_id, col)`  | `sext` + `call i32 @parse_{id}(ptr %table, i64 %col)` | Call rule function (which checks memo table internally). Returns match length or negative. |

### Backtrack stack

At the beginning of lexing, malloc a 1M `i32 * 256K` stack as local variable.

And all scope's backtracking can share this same stack.

| Operation                              | Semantics                                                   |
|----------------------------------------|-------------------------------------------------------------|
| `define internal @save(ptr %stack, i32 %col)` | Push column position onto backtrack stack.           |
| `define internal @restore(ptr %stack)` | Read saved column without popping. Returns the saved `col`. |
| `define internal @discard(ptr %stack)` | Drop top of backtrack stack.                                |

The backtrack stack type and operations are emitted as `define internal` functions in the generated LLVM IR, so LLVM can inline them.

# Code gen

`gen(pattern, col, on_fail)` generates IR for matching `pattern` at column `col`, branching to `on_fail` on failure. Returns match length on success.

### Primitives Translation

```
gen(empty, col, fail):
  return 0

gen(token, col, fail):
  br(token_id == tokens[col].token_id, good, fail)
good:
  ret 1
fail:
  ret -1

gen(Rule, col, fail):
  ret call(Rule, col)
```

### Sequence

```
gen(a b, col, fail):
  r1 = gen(a, col, fail)
  r2 = gen(b, col + r1, fail)
  return r1 + r2
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
  result = phi(r from succ_bb, r2 from alt_bb)
  return result
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
  result = phi(r from try_bb, 0 from miss_bb)
  return result
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
  return acc
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
  return acc
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
  return acc
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
  return result
```

# Notes

- `gen` always has an `on_fail` label. On failure, control transfers there. On success, it falls through and returns the match length.
- `?` and `*` always succeed — they never branch to `on_fail`.
- `+` and `*` are possessive (no backtracking).
- `+<sep>` / `*<sep>` interlace: the separator is only consumed when followed by a successful element match. The loop exits before accumulating the separator, discarding both `sr` and `er`.
