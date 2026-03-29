# Parsing expression grammar

src/peg.c is a packrat parsing generator.

create `src/peg.c` (`peg_gen()`):
- define generation logic for different PEG constructs, and generate LLVM IR, using Parser's processed-data
- generation helpers for result C header (reference the "Using the generated code" section below):
  1. node definition
  2. node extraction functions
  3. memoize table construction helpers for LLVM IR to use

It iterates parsed PEG structure, utilize src/re.h to generate code.

It assigns rule ids for each scope.

It provides 2 generating options: naive & row_shared, so we can benchmark.

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

Each slot:
- if rule is sequence rule, stores match length of the rule (some rule can be 0-sized so 0 is a value).
- if rule is branch rule, store the chosen branch id.
- if rule is chainable (a+), store the offset to next token.

# `row_shared` mode

Rule IDs can share a slot storage when 2 rules do not co-exist at one matching position.

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
  - `struct Col { int32_t bits[nseg_groups]; int32_t slots[slot_size]; }`
- init state: set all bits & slots to 1 `memset(peg_table, -1 /* 0xFF */, table_bytes)`
- for a rule, we know:
  - the segment it belongs to: `segment_bits = bits[segment_index] & segment_mask`
  - check the bit `segment_bits & rule_bit`
    - if rule bit is `1`, it may match, then we:
      - check the slot, if not `-1`, then the rule matches.
      - else do the real parse and write cache:
        - if rule matches, set all other bits in the segment to 0 (remember exclusive right?) and set slot.
        - else deny the rule bit, set it to `0`.
    - if rule bit is `0`, it means previous tries cached the failure, rule does not match.

For performance of generated code, same-group bitset should be segmented by 32. see coloring.md for more details.

# Code gen

- llvm IR to handle the PEG parsing, in the same module as vpa. See also [peg_ir.md](peg_ir.md) for isolation of concerns.
- packrat parsing: a chunk-allocated parsing table
- when vpa pops, we know how many columns the table needs and allocate the chunk and invoke the corresponding peg parser on the table segment
- util functions like memoize table alloc, should be in C header, write with header_writer
- AST node management should also be in the C header

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
