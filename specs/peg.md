# Parsing expression grammar

src/peg.c is a packrat parsing generator.

create `src/peg.c` (`void peg_gen(PegGenInput* input, HeaderWriter* hw, IrWriter* w, bool compress_memoize, const char* prefix)`):
- `prefix` comes from `nest` command arg `-p`.
- gather scope closures (see below)
- define generation logic for different PEG constructs, and generate LLVM IR, using Parser's processed-data
- generation helpers for result C header (reference the "Using the generated code" section below):
  1. node definition
  2. node extraction functions
  3. memoize table construction helpers for LLVM IR to use

It iterates parsed PEG structure, utilize src/re.h to generate code.

It assigns rule ids for each scope.

It provides 2 generating options: naive & row_shared (--compress-memoize option in nest), so we can benchmark.

### Input format

```c
typedef enum {
  PEG_CALL = 1,
  PEG_TERM,
  PEG_BRANCHES,
  PEG_SEQ,
} PegUnitKind;

typedef struct PegUnit PegUnit;

typedef PegUnit* PegUnits; // darray

struct PegUnit {
  PegUnitKind kind;

  // PEG_CALL: callee rule's global_id (callee must not have a scope associated to it)
  // PEG_TERM: token id (keyword literals already expanded) | scope id
  int32_t id;

  char multiplier; // '?','+','*', or 0
  PegUnitKind interlace_rhs_kind; // 0 | PEG_CALL | PEG_TERM
  int32_t interlace_rhs_id; // calee rule's global_id | token id | scope id

  char* tag;         // (owned, may be NULL)
  PegUnits children;
};

typedef struct {
  // name = symtab_find(rule_names, global_id)
  int32_t global_id;
  // scope_id = symtab_intern(scope_names, name)
  int32_t scope_id; // -1 for non-scope
  PegUnit seq;
} PegRule;

typedef PegRule* PegRules; // darray

typedef struct {
  PegRules rules;
  Symtab tokens;      // owned by ParseState
  Symtab scope_names; // owned by ParseState
  Symtab rule_names;  // owned by ParseState
} PegGenInput;
```

### Scope closures

Create function `_gather_scope_closures()`, which gather rules for each scope:

- same rule within different scopes will have their independent numbering
  - for example, scope `s1 = r1` and scope `s2 = r2`, r1 & r2 may have different numbering in this 2 scopes because we are parsing in a divide-and-conqur manner. Scope is the division unit
- to gather rules for each scope, we recursively walk down the scope's definition, expanding sub rules and sub-sub rules and go on.
- the purpose of this closure-finding is to make each scope's parsing table minimum.

Then we will have this info:

```c
struct ScopeClosure {
  const char* scope_name;
  Symtab defined_rules;
  ...
};
```

### Breakdown rules

Create funciton `_breakdown_rules()`, will generates fine-grained rules.

For example, this composite rule:

```
foo = a [
  b
  c
] [
  e
  f
]

# Assume only `c` is a branch rule, b/e/f are non-branch rules
```

Then `foo` should be separated to:

```
foo = seq(a foo$1 foo$2)
foo$1 = branches(b, foo$1$2)
foo$1$2 = seq(c) # wrap in seq, avoid calling branch from branch directly
foo$2 = branches(e, f)
```

One nuance is:
- if a branch rule calls another branch rule, wrap the child rule in a seq
- this wrapping ensures an invariance: to find out a branch rule's cached consumed token size, we can first read the slot to get the child rule id (`foo$1$2` for the above example), and read the size from directly -- without the need of further drill-down.

After closure analysis & rule breakdown, we will have the following structure which codegen can use easily:

```c
enum ScopedRuleKind {
  SCOPED_RULE_KIND_BRANCHES,
  SCOPED_RULE_KIND_SEQ,
  SCOPED_RULE_KIND_CALL,
  SCOPED_RULE_KIND_JOIN,
  SCOPED_RULE_KIND_TERM,
};

struct ScopedRule {
  const char* name; // for example: "foo$1$2"
  ScopedRuleKind kind;
  union {
    int32_t* branches; // darray of scoped_rule_ids
    int32_t* seq;      // darray of scoped_rule_ids
    int32_t call;      // scoped_rule_id
    {int32_t, int32_t} join; // {lhs, rhs}, maps to lhs*<rhs> / lhs+<rhs>
    int32_t term;      // term (token id | scope id)
  } as;
  char multiplier; // ?, *, +
  DebugInfo di; // maps to source code

  // ... analysis|codegen props, see below
};

struct ScopeClosure {
  const char* scope_name;
  Symtab defined_rules;
  ScopedRule* rules; // darray of broken down rules
};
```

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

Each slot stores:

- if it is a branch rule, store the chosen branch's scope_rule_id
  - to extract the parsed token size of a branch rule, get the `slot_index` of child `scope_rule_id`, then read it.
- for other rule, stores the token size of the matching rule. All slots initialized to `-1`: means we don't know the match yet.

# `row_shared` mode

Rule IDs can share a slot storage when 2 rules do not co-exist at one matching position. We need extra bits to denote what the slot means.

### Exclusiveness analysis

Compute `first_set(R)` and `last_set(R)` for each rule R, expanding references to leaf token id sets.

Compute `max_size(R)` and `min_size(R)` for each rule R, use `-1` for unlimited size.

Note that if a rule being called is also a scope, we should not expand it in first_set/last_set computation, just add the scope_id to the set.

Two rules A, B are **exclusive** if:

OR
- AND 
  + `min_size(A) > 0`
  + `min_size(B) > 0`
  + `first_set(A) ∩ first_set(B) = ∅` // this also covers the `all_set` intersection case
- AND
  + `min_size(A) == max_size(A) == min_size(B) == max_size(B) > 0`
  + `last_set(A) ∩ last_set(B) = ∅` // no need compare `first_set`, it is already checked

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
  - check the bit `rule_bit = segment_bits & rule_bit_mask`
    - if rule bit is `1`, it may match, then we:
      - check the slot, if not `-1`, then the rule matches.
      - else do the real parse and write cache:
        - if rule matches, set all other bits in the segment to 0 (remember exclusive right?) and set slot.
        - else deny the rule bit, set it to `0`.
    - if rule bit is `0`, it means previous tries cached the failure, rule does not match.

For performance of generated code, same-group bitset should be segmented by 32. see coloring.md for more details.

After analysis, we have these information for a rule in a scope (different in other scope because per-scope numbering):

```c
struct ScopedRule {
  // ... basic props, see above

  bool nullable;    // can the rule match 0-length token?
  Bitset first_set; // excluding epsilon
  Bitset last_set;  // excluding epsilon

  uint32_t scoped_rule_id; // unique in a scope closure
  uint32_t segment_index;  // check Col.bits[segment_index]
  uint32_t segment_mask;
  uint32_t rule_bit_mask;  // single bit in segment_mask
  uint32_t slot_index;     // Col.slots[slot_index] is the matched token size, same segment, same slot_index
};
```

# Code gen

`define internal parse_{scope_name}(TokenTree* tt, stack_ptr)`: callable from the IR generated by [VPA](vpa.md) because they live in the same IRWriter.

In the funciton, we allocate & init table for the scope, then lower to fine-graned parsing procedures:
```pseudo
%tc = tt_current(tt)
%table_size = tt_current_size(tt) * sizeof_col
%peg_table = malloc(%table_size)
memset(%peg_table, -1 /* 0xFF */, %table_size)
%tc->value = %peg_table
br {scope_name}$foo

{scope_name}$foo:
  ; access memoize table
  ; generated by peg_ir

{scope_name}$foo$1:
  ; access memoize table
  ; generated by peg_ir

...

ret %parse_success
```

To access memoize table:

- Read memoize table slot
  - `table->col[%col].slots[%slot_index]`
- Write memoize table slot
  - `table->col[%col].slots[%slot_index] = val`

In `row_shared` mode, we also have:

- `define internal @bit_test(seg_idx, rule_bit, col)`: Test rule's bit in the segment. Returns `i1` (1 = may match, 0 = proven fail).
  - LLVM: `getelementptr` + `load` + `and i32 %bits, %rule_bit` + `icmp ne i32 ..., 0`
- `define internal @bit_deny(seg_idx, rule_bit, col)`: Clear rule's bit (cache failure).
  - LLVM: `getelementptr` + `load` + `and i32 %bits, ~%rule_bit` + `store`
- `define internal @bit_exclude(seg_idx, rule_bit, col)`: Keep only this rule's bit, zero out others in segment (cache exclusive match).
  - LLVM: `getelementptr` + `load` + `and i32 %bits, %rule_bit` + `store`

In the end the memoize table is associated to each `TokenChunk`.

# User API: parse tree retrieval

When parse succeeds, VPA will return a PegRef.

- have the full memoize table in order to construct the parse tree
  - there are 2 kinds of types: 
    - a universal reference `PegRef { TokenChunk* tc, int32_t col, int32_t next_col }`
      - `tc->value` is the memoize table
    - rule-specific nodes `FooNode { struct { bool branch1: 1; bool branch2: 1; } is; PegRef child0; PegRef child1; ... }`.
  - fields are reference to a table position, with `is` field to tell the branch
  - by the `is` user can call a helper function defined in generated header, to extract child node from the memoize table

A typical using pattern is:

```c
PegRef ref = ...;
FooNode foo = {prefix}_load_foo(ref);
// foo = [
//   bar+ : tag1
//   baz bar : tag2
// ]
if (foo.is.tag1) {
  for (elem = foo.bar; has_next(elem); elem = get_next(elem)) {
    bar = {prefix}_load_bar(elem);
    ...
  }
} else if (foo.is.tag2) {
  baz = {prefix}_load_baz(foo.baz);
  bar = {prefix}_load_bar(foo.bar);
}
```

Note that `{prefix}_load_{decl_rule_name}` works on defined rules, no need to generate loaders for broken-down rules.

# Error handling in generated code

TODO: implement cut operator `^`, when no terminal can match after cut, report cut error.

# Acceptance criteria

- end-to-end test: create a terminal list (you can mimic json, for example), use generated code, to parse the list, and produce a memoize table, by the parsed memoize table, we can use the generated `{prefix}_load_{decl_rule_name}()` function to retrieve/drill-down the nodes, and the loading doesn't allocate heap memory at all.
- resulting memoize tables: assume there are 5 rules in scope A, 6 rules in scope B, resulting memoize tables should have rows 5 and 6 in each, not full row heights each.
- resulting load_xxx MUST NOT call parse_xxx functions, just decode from memoize table.
