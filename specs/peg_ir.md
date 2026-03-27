To help generate the PEG IR easier, create `peg_ir_xxx` helpers in src/peg_ir.c, which wraps irwriter.

The PEG IR is a thin abstraction over LLVM IR. Each opcode maps directly to a small sequence of LLVM instructions. The goal is to lessen the burden of IR generating in src/peg.c — PEG constructs call these helpers instead of emitting raw LLVM IR.

## Opcodes

All values are `i32`. Convention: negative return = failure.

| Opcode | LLVM IR expansion | Semantics |
|---|---|---|
| `tok(token_id, col)` | `call i32 @match_tok(i32 %token_id, i32 %col)` | Match token at `col`. Returns match length (≥0) or negative on failure. |
| `call(rule_id, col)` | `call i32 @rule_{id}(ptr %table, i32 %col)` | Call rule function (which checks memo table internally). Returns match length or negative. |
| `memo_get(rule_id, col)` | `getelementptr` + `load i32` from `table->col[%col].slots[%rule_id]` | Read memo slot. Returns cached value or `-1` (uncached). |
| `memo_set(rule_id, col, val)` | `getelementptr` + `store i32 %val` into `table->col[%col].slots[%rule_id]` | Write memo slot. |
| `fail_if_neg(val, fail_bb)` | `%cmp = icmp slt i32 %val, 0` / `br i1 %cmp, label %fail_bb, label %cont` | Branch to `fail_bb` if `val < 0`. |
| `is_neg(val)` | `%cmp = icmp slt i32 %val, 0` | Returns `i1`, true when `val < 0`. |
| `select(cond, a, b)` | `select i1 %cond, i32 %a, i32 %b` | Conditional value. |
| `add(a, b)` | `add i32 %a, %b` | Integer add (accumulate match lengths). |
| `phi(...)` | `phi i32 [%v1, %bb1], [%v2, %bb2], ...` | SSA merge at join point. |
| `br(bb)` | `br label %bb` | Unconditional branch. |
| `br_cond(cond, then_bb, else_bb)` | `br i1 %cond, label %then_bb, label %else_bb` | Conditional branch. |
| `const(n)` | `i32 n` | Integer literal. |

### Backtrack stack (ordered choice only)

| Opcode | Semantics |
|---|---|
| `save(col)` | Push parser state (column position) onto backtrack stack. |
| `restore()` | Read saved column without popping. Returns the saved `col`. |
| `discard()` | Drop top of backtrack stack (after successful alternative or after restore). |

The backtrack stack stores the column position. `save`/`restore`/`discard` replace the old `push`/`peek`/`pop` names to clarify intent.

The generator emit opcode definitions in LLVM-IR first (local procedures that can be inlined), then emit PEG rule implementations that utilize the functions.

### Memo helpers (row_shared mode)

These are used only in `row_shared` mode. In `naive` mode, `memo_get`/`memo_set` suffice.

| Opcode | LLVM IR expansion | Semantics |
|---|---|---|
| `bit_test(seg_idx, rule_bit, col)` | `getelementptr` + `load` + `and i32 %bits, %rule_bit` + `icmp ne i32 ..., 0` | Test rule's bit in the segment. Returns `i1` (1 = may match, 0 = proven fail). |
| `bit_deny(seg_idx, rule_bit, col)` | `getelementptr` + `load` + `and i32 %bits, ~%rule_bit` + `store` | Clear rule's bit (cache failure). |
| `bit_exclude(seg_idx, rule_bit, col)` | `getelementptr` + `load` + `and i32 %bits, %rule_bit` + `store` | Keep only this rule's bit, zero out others in segment (cache exclusive match). |

## Reference IR

`gen(pattern, col, on_fail)` generates IR for matching `pattern` at column `col`, branching to `on_fail` on failure. Returns match length on success.

### Primitives Translation

```
gen(empty, col, fail):
  return const(0)

gen(token, col, fail):
  r = tok(token, col)
  fail_if_neg(r, fail)
  return r

gen(Rule, col, fail):
  r = call(Rule, col)
  fail_if_neg(r, fail)
  return r
```

### Sequence

```
gen(a b, col, fail):
  r1 = gen(a, col, fail)
  r2 = gen(b, add(col, r1), fail)
  return add(r1, r2)
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
  br(done_bb)
done_bb:
  result = phi(r from succ_bb, r2 from alt_bb)
  return result
```

### Optional (?)

Always succeeds, never branches to fail.

```
gen(e?, col, fail):
  r = gen(e, col, miss_bb)
  br(done_bb)
miss_bb:
  br(done_bb)
done_bb:
  result = phi(r from try_bb, const(0) from miss_bb)
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
  r = gen(e, add(col, acc), end_bb)
  next = add(acc, r)
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
  acc = phi(const(0) from entry_bb, next from body_bb)
  r = gen(e, add(col, acc), end_bb)
  next = add(acc, r)
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
  sr = gen(sep, add(col, acc), end_bb)
  er = gen(e, add(add(col, acc), sr), end_bb)
  next = add(acc, add(sr, er))
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
  sr = gen(sep, add(col, acc), end_bb)
  er = gen(e, add(add(col, acc), sr), end_bb)
  next = add(acc, add(sr, er))
  br(loop_bb)
empty_bb:
  br(end_bb)
end_bb:
  result = phi(acc from loop_bb, const(0) from empty_bb)
  return result
```

## Notes

- `gen` always has an `on_fail` label. On failure, control transfers there. On success, it falls through and returns the match length.
- `?` and `*` always succeed — they never branch to `on_fail`.
- `+` and `*` are possessive (no backtracking).
- `+<sep>` / `*<sep>` interlace: the separator is only consumed when followed by a successful element match. The loop exits before accumulating the separator, discarding both `sr` and `er`.
- In `row_shared` mode, rule functions use `bit_test`/`bit_deny`/`bit_exclude` before `memo_get`/`memo_set`. In `naive` mode, they use `memo_get`/`memo_set` directly.
