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
  int verbose_level;  // passed from from cli.md
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
  - so the maybe/star/plus can have a bit (like defined rules) like other user defined rules and have the total parsed size cached
  - state after parsing:
    - if the bit is not set, or the first elem doesn't match, it means `has_elem() = false`
    - else `has_elem() = true`
  - check [PEG IR](peg_ir.md) and the `has_elem` and `get_next` functions in [PEG GEN](peg_gen.md) for more details about the single-bit linking works.
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
- matched token size for all rules (defined & multipliers)
- tags for defined rules, so we know which branches are chosen, it can also be assigned to resulting node

We have 3 memoize_modes (passed from [cli](cli.md)): `none`, `naive`, `shared`.

Slot bits and tag bits are allocated per `ScopeClosure` by [peg_alloc](peg_alloc.md) — that module owns:
- per-rule analysis (`nullable`, `min_size`, `max_size`, `first_set`, `last_set`)
- naive-mode tag bit knapsack packing
- shared-mode exclusiveness analysis, interference graph + coloring, logical and physical layout
- the final `Col${scope_name}` structure and the `ScopedRule` allocation fields (`tag_bit_index`, `tag_bit_offset`, `tag_bit_count`, `segment_color`, `segment_index`, `segment_mask`, `rule_bit_mask`, `slot_index`)

`peg_analyze` just calls `peg_alloc_scope(closure, memoize_mode)` once per closure after gathering it.

See [peg_alloc](peg_alloc.md) for the full spec (modes, slot encoding semantics, packing rules, acceptance criteria).

# Call site analysis

Compute for each scoped rule the call sites that target it. A single caller rule can call the same callee multiple times (e.g. `value*<@comma>` generates two call sites to `value`). This is used by [peg_ir](peg_ir.md) to emit correct `indirectbr` destination lists.

Create function `_compute_call_sites()` after all other analysis passes. Walk each `ScopedRule.body` tree. For each `SCOPED_UNIT_CALL` node, append a `CallSite` to the callee's list. Also record the entrance call (the scope calls `scoped_rules[0]`).

```c
typedef struct {
  int32_t caller_id; // scoped_rule index of the caller
  int32_t site;      // nth call site within that caller (0-based)
} CallSite;
```

Store in `ScopedRule`:

```c
CallSite* call_sites; // darray
```

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
  int32_t branch_index;     // branch tag index (0-based), -1 for non-branch fields
} NodeField;

typedef NodeField* NodeFields; // darray
```

This is stored in `ScopedRule.node_fields` (NULL for generated sub-rules).

Field names are sanitized (`@` and `.` replaced with `_`) and deduplicated (second occurrence becomes `name$1`, third `name$2`, etc.).

Code generation and user API are specified in [peg_gen](peg_gen.md).

# Output

`peg_analyze` returns a [PegGenInput](peg_gen.md) struct for code generation. Call `peg_analyze_free()` to release the analyzed closures when done.

# Verbose level

When verbose level > 1, print number of scopes, number of tag bits, color group allocations, final column layout for the memoize table.

# Acceptance criteria

- end-to-end test: create a terminal list (you can mimic json, for example), use generated code, to parse the list, and produce a memoize table, by the parsed memoize table, we can use the generated `{prefix}_load_{decl_rule_name}()` function to retrieve/drill-down the nodes, and the loading doesn't allocate heap memory at all.
- if spec already given a variable/function name, don't re-invent yet another name for the same idea.
- resulting memoize tables: assume there are 5 rules in scope A, 6 rules in scope B, resulting memoize tables should have rows 5 and 6 in each, not full row heights each.
- When memoize_mode=naive/shared, resulting load_xxx MUST NOT call parse_xxx functions, just decode from memoize table.
