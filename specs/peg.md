Parsing expression grammar

src/peg.c is a packrat parsing generator.

It iterates parsed PEG structure, utilize src/re.h to generate code.

It provides 2 generating options: naive & row_shared, so we can benchmark.

# `naive`

### Analyze

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

# `row_shared`

Rule IDs can share a slot storage when 2 rules do not co-exist at one matching position.

### Exclusiveness analysis

Compute `first_set(R)` and `last_set(R)` for each rule R, expanding references to leaf token id sets.

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
- init state: set all bits & slots to 1 `memset(peg_table, table_bytes, -1)`
- for a rule, we know:
  - the segment it belongs to: `segment_bits = bits[segment_index] & segment_mask`
  - check the bit `segment_bits & rule_bit`
    - if rule bit is `1`, it may match, so we try some little more options:
      - check the slot, if not `-1`, then the rule matches.
      - else do the real parse and write cache:
        - if rule matches, set all other bits in the segment to 0 (remember exclusive right?) and set slot.
        - else deny the rule bit, set it to `0`.
    - if rule bit is `0`, it means previous tries cached the failure, rule does not match.

For performance of generated code, same-group bitset should be segmented by 32. see coloring.md for more details.

# Code gen

- llvm IR in the same module as vpa
- packrat parsing: a chunk-allocated parsing table
- when vpa pops, we know how many columns the table needs and allocate the chunk and invoke the corresponding peg parser on the table segment

# Parse tree representation

- have the full memoize table in order to construct the parse tree
  - there are 2 kinds of types: 
    - a universal reference `PegRef { table, col, next_col }`
    - rule-specific nodes `FooNode { struct { bool branch1: 1; bool branch2: 2; } is; PegRef child0; PegRef child1; ... }`.
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
