# Parsing expression grammar

src/peg.c is a packrat parsing generator.

create `src/peg.c` (`peg_gen()`):
- gather scope closures (see below)
- define generation logic for different PEG constructs, and generate LLVM IR, using Parser's processed-data
- generation helpers for result C header (reference the "Using the generated code" section below):
  1. node definition
  2. node extraction functions
  3. memoize table construction helpers for LLVM IR to use

It iterates parsed PEG structure, utilize src/re.h to generate code.

It assigns rule ids for each scope.

It provides 2 generating options: naive & row_shared, so we can benchmark.

### Scope closures

`_gather_scope_closures()` will gather rules for each scope:

- same rule within different scopes will have their independent numbering
  - for example, scope `s1 = r1` and scope `s2 = r2`, r1 & r2 may have different numbering in this 2 scopes because we are parsing in a divide-and-conqur manner. Scope is the division unit
- to gather rules for each scope, we recursively walk down the scope's definition, expanding sub rules and sub-sub rules and go on. but we don't expand scopes.
- the purpose of this closure-finding is to make each scope's parsing table minimum.

### Breakdown rules

For example, this composite rule:

```
foo = a [
  b
  c
] [
  e
  f
]
```

should be separated to:

```
foo = a foo$1 foo$2 # seq
foo$1 = [ # ordered choice
  b
  c
]
foo$2 = [ # ordered choice
  e
  f
]
```

Then for each scope, define a parse function. For labels inside each rule, prefix the label with rule name so they won't conflict

```
foo_rule$start:
...
foo_rule$loop_bb:
...
foo_rule$empty_bb:
...
foo_rule$end:
```

### Rule id analysis

We gather rule closures by scopes first.

```c
struct Peg {
  Map<int32_t, PegClosure> pegs; // {scope_id => peg_closure}
  Map<string, Rule> rules; // {rule_name => definition}
};

struct PegClosure {
  Map<string, int32_t> rule_ids; // {rule_name => assigned (compact) id in closure}
};
```

Rule_id minification
- in each scope we gather a set of rules, and number them (starting from 1)

# `naive` mode

### Runtime Table

Parsing table layout:

```c
struct ScopedTable {
  Col col[token_size];
};

struct Col {
  int32_t slots[slots_size];
};
```

Each slot stores the token size of the matching rule. All slots initialized to `-1`: means we don't know the match yet.

# `row_shared` mode

Rule IDs can share a slot storage when 2 rules do not co-exist at one matching position. We need extra bits to denote what the slot means.

### Exclusiveness analysis

Compute `first_set(R)` and `last_set(R)` for each rule R, expanding references to leaf token id sets.

Note that if a rule being called is also a scope, we should not expand it in first_set/last_set computation, just add the scope_id to the set.

Two rules A, B are **exclusive** when `first_set(A) ∩ first_set(B) = ∅` or `last_set(A) ∩ last_set(B) = ∅`.

### Interference graph and coloring

Build an interference graph G where:
- each rule in the closure is a vertex
- add an edge (A, B) when A and B are **not** exclusive (they may co-exist at the same position)

Graph-color G so that vertices sharing an edge get different colors. Each color = one slot in the memoize table.

Use kissat (vendored SAT solver) to encode the k-coloring problem:
- a simple optimization: break the symmetry

### Slot encoding

After graph coloring, we have shared-groups (sets of peg rule ids).

Then we use reverse-bitset representation to denote what each slot means:
- the bit map co-lives with cache slots in one single struct:
  - `struct Col { int32_t bits[nseg_groups]; int32_t slots[slot_size]; int32_t aux[slot_size]; }`
- init state: set all bits & slots to 1 `memset(peg_table, -1 /* 0xFF */, table_bytes)`
- for a rule, we know:
  - the segment it belongs to: `segment_bits = bits[segment_index] & segment_mask`
  - check the bit `rule_bit = segment_bits & rule_bit_mask`
    - if rule bit is `1`, it may match, then we:
      - check the slot, if not `-1`, then the rule matches.
      - else do the real parse and write cache:
        - if rule matches, set all other bits in the segment to 0 (remember exclusive right?) and set slot.
        - else deny the rule bit, set it to `0`.
    - if rule bit is `0`, it means previous tries cached the failure, rule does not match.

`aux[slot_index]` stores additional data for different kinds of rules:
- For a branch rule, it stores the `child_scoped_rule_id`
- If in definition this rule can repeat, it stores `next_offset` or `-1`
  - for example `a = b*<c>`, then `b`'s `next_offset` can help calculate next b in the chain, `c`'s `next_offset` can help calculate next `c` in the chain.
  - for a more complex example `a = b @c b* @d b`, then the first `b`, `b*` and the third `b` are all chained together by `next_offset`.

For performance of generated code, same-group bitset should be segmented by 32. see coloring.md for more details.

After analysis, we have these information for a rule in a scope (different in other scope because per-scope numbering):

```c
struct ScopedRule {
  uint32_t scoped_rule_id; // unique in a scope closure
  uint32_t segment_index; // check Col.bits[segment_index]
  uint32_t segment_mask;
  uint32_t rule_bit_mask; // single bit in segment_mask
  uint32_t slot_index;    // Col.slots[slot_index] is the matched token size
};
```

# Code gen

- llvm IR to handle the PEG parsing, in the same module as vpa. See also [peg_ir.md](peg_ir.md) for isolation of concerns.
- packrat parsing: a chunk-allocated parsing table
- when vpa pops, we know how many columns the table needs and allocate the chunk and invoke the corresponding peg parser on the table segment
- util functions like memoize table alloc, should be in C header, write with header_writer
- AST node management should also be in the C header

Memoize table ops

- Read memoize table slot
  - `table->col[%col].slots[%slot_index]`
- Write memoize table slot
  - `table->col[%col].slots[%slot_index] = val`

`row_shared` mode only

- `define internal @bit_test(seg_idx, rule_bit, col)`: Test rule's bit in the segment. Returns `i1` (1 = may match, 0 = proven fail).
  - LLVM: `getelementptr` + `load` + `and i32 %bits, %rule_bit` + `icmp ne i32 ..., 0`
- `define internal @bit_deny(seg_idx, rule_bit, col)`: Clear rule's bit (cache failure).
  - LLVM: `getelementptr` + `load` + `and i32 %bits, ~%rule_bit` + `store`
- `define internal @bit_exclude(seg_idx, rule_bit, col)`: Keep only this rule's bit, zero out others in segment (cache exclusive match).
  - LLVM: `getelementptr` + `load` + `and i32 %bits, %rule_bit` + `store`

# Parse tree representation

- have the full memoize table in order to construct the parse tree
  - there are 2 kinds of types: 
    - a universal reference `PegRef { table, col, next_col }`
    - rule-specific nodes `FooNode { struct { bool branch1: 1; bool branch2: 1; } is; PegRef child0; PegRef child1; ... }`.
  - fields are reference to a table position, with `is` field to tell the branch
  - by the `is` user can call a helper function defined in generated header, to extract child node from the memoize table

A typical using pattern is:

```c
PegRef ref = ...;
FooNode foo = load_foo(ref);
// foo = [
//   bar+ : tag1
//   baz bar : tag2
// ]
if (foo.is.tag1) {
  for (elem = foo.bar; has_next(elem); elem = get_next(elem)) {
    bar = load_bar(elem);
    ...
  }
} else if (foo.is.tag2) {
  baz = load_baz(foo.baz);
  bar = load_bar(foo.bar);
}
```

Note that `load_xxx` works on defined rules, no need to generate loaders for broken-down rules.

# Acceptance criteria

- end-to-end test: create a token list (you can mimic json, for example), use generated code, to parse the list, and produce a memoize table, by the parsed memoize table, we can use the generated `load_xxx()` function to retrieve/drill-down the nodes, and the loading doesn't allocate heap memory at all.
- resulting memoize tables: assume there are 5 rules in scope A, 6 rules in scope B, resulting memoize tables should have rows 5 and 6 in each, not full row heights each.
- resulting load_xxx MUST NOT call parse_xxx functions, just decode from memoize table.
