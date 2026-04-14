# PEG analysis

src/peg_analyze.c analyzes PEG structure, and can be used by [peg_gen](peg_gen.md).

create function `PegGenInput peg_analyze(PegAnalyzeInput* input, int memoize_mode, const char* prefix)`:
- `prefix` comes from `nest` command arg `-p`.
- gather scope closures (see below)
- define generation logic for different PEG constructs, and generate LLVM IR, using Parser's processed-data
- generation helpers for result C header (reference the "Using the generated code" section below):
  1. node definition
  2. node extraction functions
  3. memoize table construction helpers for LLVM IR to use

It iterates parsed PEG structure, utilize [peg_ir](peg_ir.md) to generate code.

It assigns rule ids for each scope.

It provides 3 memoize_mode options: none, naive & shared (from [cli](cli.md)), so we can benchmark to compare the 3 modes.

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

// NOTE: as result of closure analysis id/interlace_rhs_id becomes scoped_rule_id
struct PegUnit {
  PegUnitKind kind;

  // PEG_CALL: callee rule's global_id (callee must not have a scope associated to it)
  // PEG_TERM: token id (keyword literals already expanded) | scope id
  int32_t id;

  char multiplier; // '?','+','*', or 0
  PegUnitKind interlace_rhs_kind; // 0 | PEG_CALL | PEG_TERM
  int32_t interlace_rhs_id; // calee rule's global_id | token id | scope id

  char* tag;         // (owned, may be NULL)
  PegUnits children; // seq members or branch members; may be NULL (darray_size handles NULL → 0)
};

typedef struct {
  // name = symtab_find(rule_names, global_id)
  int32_t global_id;
  // scope_id = symtab_intern(scope_names, name)
  int32_t scope_id; // -1 for non-scope
  int32_t source_line; // line in .nest file (for LLVM IR debug info), 0 = unknown
  int32_t source_col;
  PegUnit body;
} PegRule;

typedef PegRule* PegRules; // darray

typedef struct {
  PegRules rules;
  Symtab tokens;      // owned by ParseState
  Symtab scope_names; // owned by ParseState
  Symtab rule_names;  // owned by ParseState
  int verbose;        // verbose level, see also cli.md
} PegAnalyzeInput;
```

### Scope closures

Create function `_gather_scope_closures()`, which gather rules for each scope:
- same rule within different scopes will have their independent numbering
  - for example, scope `s1 = r1` and scope `s2 = r2`, r1 & r2 may have different numbering in this 2 scopes so we can minimize rows for each scope.
- to gather rules for each scope, we recursively walk down the scope's definition, expanding sub rules and sub-sub rules and go on.
- the purpose of this closure-finding is to make each scope's parsing table minimum.

It also transforms `PegUnit` into `ScopedUnit`
- makes multipliers explicit types.
- expand the expression tree of user-defined rule
- when a seq has only 1 member, inline the member
- don't inline the branch though, resulting node requires the tags
- if it is a branch with a tag, assign `tag_offset`, which will be later used memoize caching

It breaks down multiplier rules so we can use "linked-list" in memoize table to represent them.
- if a rule contains maybe/star/plus, a child rule named `{scope_name}${rule_name}${multiplier_num}` is created
  - so the maybe/star/plus can have a cache slot, to chain the elements
  - check the `has_next` and `get_next` functions defined below to get how the linked-list works.
- analysis don't need to consider nested case, because by syntax, there won't be any multiplier inside multiplier -- we won't have labels like `foo$bar$1$2`.

Then we will have this info:

```c
typedef enum {
  SCOPED_UNIT_CALL = 1,
  SCOPED_UNIT_TERM,
  SCOPED_UNIT_BRANCHES,
  SCOPED_UNIT_SEQ,

  SCOPED_UNIT_MAYBE,
  SCOPED_UNIT_STAR,
  SCOPED_UNIT_PLUS
} ScopedUnitKind;

struct ScopedUnit;
typedef struct ScopedUnit ScopedUnit;
typedef ScopedUnit* ScopedUnits;

typedef struct {
  ScopedUnit* lhs;
  ScopedUnit* rhs;
} ScopedInterlace;

// a struct for codegen convenience
struct ScopedUnit {
  ScopedUnitKind kind;

  union {
    const char* callee; // scoped_rule_name from symtab, not owned
    int32_t term_id;
    ScopedUnits children; // for branches & seq
    ScopedUnit* base; // maybe
    ScopedInterlace interlace; // for star & plus
  } as;

  // index in tags, or -1 for non-tagged unit
  // when there is tag_offset, parsing should set the tag_bit to 1
  int32_t tag_bit_local_offset;

  // if the unit is nullable, we need advancement check for multiplier rules
  bool nullable;
} ScopedUnit;

struct ScopedRule {
  const char* scoped_rule_name; // not owned, point to ScopeClosure.scoped_rule_names
  ScopedUnit body; // tree clone of the original rule, but `id` and `interlace_rhs_id` are re-numbered as scoped_rule_ids
  Symtab tags; // total tags

  // ... analysis|codegen props, see below
};

typedef ScopedRule* ScopedRules; // darray

struct ScopeClosure {
  const char* scope_name;
  // each rule is named: "{scope_name}${rule_name}", can be used as IR label
  Symtab scoped_rule_names;
  ScopedRules scoped_rules; // defined rules
  int32_t source_line; // line in .nest file (from PegRule), for LLVM IR debug info
  int32_t source_col;

  // after analysis below
  int64_t bits_bucket_size;
  int64_t slots_size;
};
```

# Packrat parsing

Defined rules can be memoized to make the parsing O(n).

What to memoize:
- for maybe/star/plus rules:
  - element token size (so we can iterate to next element)
  - element token size --(after size)-> next element token size --(after size)-> ... -> -1 (end of match)
  - for interlaced stat/plus rules, `element` token size means `lhs` token size
- for other rules:
  - matched token size
- tags, so we know which branches are chosen, it can also be assigned to resulting node

To map tags to bits, create function `_alloc_tag_bits()` to assign tag bits to each rule:
- For each scope closure
  - For each `body` in `PegAnalyzeInput.rules`
    - create symtab `ScopedRule.tags` and give a number to all its tags
  - Now we know how many bits per scoped_rule costs, arrange them to `uint64_t` buckets so that the required buckets are minimal
    - this is a knapsack problem, we use a simple greedy algorithm to allocate them

After `_alloc_tag_bits`, we have:

```c
struct ScopedRule {
  // ... basic props, see above

  uint64_t tag_bit_index;
  uint64_t tag_bit_mask;
  uint64_t tag_bit_offset;
}
```

We have 3 memoize_modes (passed from [cli](cli.md)).

## `memoize_mode=none`

TODO: parsing not using memoize table.

## `memoize_mode=naive`

### Runtime Table

Parsing table layout (runtime structs):

```c
struct Col${scope_name} {
  uint64_t bits[{bits_bucket_size}];
  int32_t slots[{scoped_rule_size}];
};
```

When memoize happens:
- the token size of the matching rule. All slots initialized to `-1`: means we don't know the match yet.
- in naive mode, match failure is stored as `-2` (but we don't store/check `-2` in shared mode, see details below).

## `memoize_mode=shared`

Basic idea: rules can share a slot & tagbits storage when they do not co-exist at one matching position (exclusiveness).

We need some extra bits to denote what the slot & tagbits position means. And these bits can be packed together with tagbits.

Create function `_alloc_slot_bits()`, to finish the following analysis.

### Exclusiveness analysis

Compute `first_set(R)` and `last_set(R)` for each rule R, expanding references to leaf token id sets.

Compute `max_size(R)` and `min_size(R)` for each rule R, use `UINT32_MAX` for unlimited size.

Note that rules may recursively call each other, we need fix-point analysis.

Two rules A, B are **exclusive** if one of following holds:

- `min_size(A) > 0 and min_size(B) > 0 and first_set(A) ∩ first_set(B) = ∅`
  + note: this also covers the `all_set` intersection case
- `min_size(A) == max_size(A) == min_size(B) == max_size(B) > 0 and last_set(A) ∩ last_set(B) = ∅`
  + note: no need compare `first_set`, it is already checked

### Interference graph and coloring

Build an interference graph G where:
- each rule in the closure is a vertex
- add an edge (A, B) when A and B are **not** exclusive (they may co-exist at the same position)

Graph-color G so that vertices sharing an edge get different colors. Each color = one slot in the memoize table.

Use kissat (vendored SAT solver) to encode the k-coloring problem:
- a simple optimization: break the symmetry

### Slot encoding

After graph coloring, we have shared-groups (sets of peg rule ids).

The Col data structure is still a similar struture

```c
struct Col${scope_name} {
  uint64_t bits[{total_buckets}]; // buckets for both slot bits and tag bits
  int32_t slots[{segment_size}];
};
```

Then we use negated-bitset representation to denote what each slot means:
- the bit map also lives in `Col.bits` like tag bits, but they don't overlap.
- init state: all bits & slots are set to 1 because `tt_alloc_memoize_table()` already did so.
- for a rule, we know:
  - the segment it belongs to: `segment_bits = bits[segment_index] & segment_mask`
  - check the bit `rule_bit = segment_bits & rule_bit_mask`
    - if rule bit is `1`, it may match, then we:
      - check the slot, if not `-1`, then the rule matches.
      - else do the real parse and write cache:
        - if rule matches, set all other bits in the segment to 0 (remember exclusive right?) and set slot.
        - else deny the rule bit, set it to `0`.
    - if rule bit is `0`, it means previous tries cached the failure, rule does not match.

For performance of generated code, same-group bitset should be segmented by 64. see [coloring](coloring.md) for more details.

`_alloc_shared_tag_bits` is executed after `_alloc_slot_bits`, filling the gaps in existing bit buckets:
- For each color group (segment), calculate the max `symtab_count(tags)`
- They can be put in the remaining bucket gaps, or create new buckets

Put together, during and after analysis, we have these information for a rule in a scope (different in other scope because per-scope numbering):

```c
struct ScopedRule {
  // ... basic props, see above

  uint64_t tag_bit_index; // same for the segment
  uint64_t tag_bit_mask; // same for the segment
  uint64_t tag_bit_offset;  // same for the segment

  bool nullable;    // can the rule match 0-length token?
  Bitset first_set; // excluding epsilon
  Bitset last_set;  // excluding epsilon

  uint64_t segment_index;  // check Col.bits[segment_index]
  uint64_t segment_mask;
  uint64_t rule_bit_mask;  // single bit in segment_mask
  uint64_t slot_index;     // Col.slots[slot_index] is the matched token size, same segment, same slot_index
};
```

And we also update `ScopeClosure.bits_bucket_size = bucket_count_after_alloc` and `ScopeClosure.slots_size = segment_count`, so the runtime `Col${scope_name}` will have this packed layout.

# Node field layout

After all analysis passes, compute `NodeField` layout for each `ScopedRule` with `original_global_id >= 0`. This precomputes all information codegen needs to emit `Node_xxx` structs and loader functions, so codegen doesn't need to walk the original `PegUnit` tree.

```c
typedef enum {
  NODE_ADVANCE_NONE = 0, // link field, no cursor advance
  NODE_ADVANCE_ONE,      // term, advance cursor by 1
  NODE_ADVANCE_SLOT,     // call/branches, advance cursor by slot value
} NodeAdvanceKind;

typedef struct {
  char* name;               // sanitized, deduped (owned)
  bool is_link;             // PegLink vs PegRef
  bool is_scope;            // term refers to a scope
  int32_t ref_row;          // PegRef.row or PegLink.elem.row
  int32_t rhs_row;          // PegLink interlace rhs row (-1 for none)
  NodeAdvanceKind advance;  // how to advance cursor after this field
  int32_t advance_slot_row; // valid when advance == NODE_ADVANCE_SLOT
} NodeField;

typedef NodeField* NodeFields; // darray
```

This is stored in `ScopedRule.node_fields` (NULL for generated sub-rules).

Field names are sanitized (`@` and `.` replaced with `_`) and deduplicated (second occurrence becomes `name$1`, third `name$2`, etc.).

Code generation and user API are specified in [peg_gen](peg_gen.md).

# Output

`peg_analyze` returns a [PegGenInput](peg_gen.md) struct for code generation. Call `peg_analyze_free()` to release the analyzed closures when done.

# Acceptance criteria

- end-to-end test: create a terminal list (you can mimic json, for example), use generated code, to parse the list, and produce a memoize table, by the parsed memoize table, we can use the generated `{prefix}_load_{decl_rule_name}()` function to retrieve/drill-down the nodes, and the loading doesn't allocate heap memory at all.
- if spec already given a variable/function name, don't re-invent yet another name for the same idea.
- resulting memoize tables: assume there are 5 rules in scope A, 6 rules in scope B, resulting memoize tables should have rows 5 and 6 in each, not full row heights each.
- When memoize_mode=naive/shared, resulting load_xxx MUST NOT call parse_xxx functions, just decode from memoize table.
