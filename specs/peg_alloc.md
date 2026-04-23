# PEG slot / tag bits allocation

src/peg_alloc.c allocates slot bits and tag bits for a single `ScopeClosure`, producing the physical memoize-table layout (`Col${scope_name}`).

This subsystem depends **only** on [coloring](coloring.md) (and basic utilities: darray, bitset, symtab). It must not depend on parse, peg_ir, peg_gen, or any other PEG module.

## Scope

For one `ScopeClosure`:

1. Compute per-rule exclusiveness info (`nullable`, `first_set`, `last_set`, `min_size`, `max_size`) via fix-point analysis.
   - `SCOPED_UNIT_AND` / `SCOPED_UNIT_NOT` (lookaheads) are always `nullable = true`, `min_size = 0`, `max_size = 0`, and contribute nothing to `first_set` / `last_set` (they consume no tokens).
2. Build an interference graph and k-color it (via [coloring](coloring.md)).
3. Allocate tag bits (mode-dependent).
4. Finalize physical layout: fill `bits[]` bucket indices, slot indices, and update `ScopeClosure.bits_bucket_size` / `ScopeClosure.slots_size`.

## API

```c
typedef enum {
  PEG_MEMOIZE_NONE = 0,
  PEG_MEMOIZE_NAIVE,
  PEG_MEMOIZE_SHARED,
} PegMemoizeMode;

// Allocate slot/tag bits for one ScopeClosure.
// On return, every ScopedRule in `closure->scoped_rules` has its
// allocation fields filled in, and `closure->bits_bucket_size` /
// `closure->slots_size` are set.
void peg_alloc_scope(ScopeClosure* closure, PegMemoizeMode mode);
```

The caller (peg_analyze) is responsible for:
- constructing the `ScopeClosure` with its `scoped_rules`,
- populating each `ScopedRule.tags` symtab before calling,
- calling `peg_alloc_scope` once per closure.

## Modes

### `PEG_MEMOIZE_NONE`

Ablation only. Layout is identical to `PEG_MEMOIZE_NAIVE` so that the same
`load_xxx` helpers work; parsing simply doesn't read the table.

### `PEG_MEMOIZE_NAIVE`

No sharing. Each rule gets its own slot and its own tag bits.

Layout:

```c
struct Col${scope_name} {
  uint64_t bits[{bits_bucket_size}];
  int32_t slots[{(scoped_rule_size + 1) / 2 * 2}]; // 64-bit aligned
};
```

Slots are initialized to `-1` (unknown). Match failure is stored as `-2`.

Tag bit packing (`_alloc_tag_bits`):
- For each `ScopedRule`, let `tag_bit_count = symtab_count(tags)`.
- **Small rules** (`tag_bit_count <= 64`): greedy knapsack into shared `uint64_t` buckets. Sort rules by `tag_bit_count` descending; place each into the first bucket with enough remaining bits. Constraint: `tag_bit_offset + tag_bit_count <= 64` in one bucket.
- **Big rules** (`tag_bit_count > 64`): bucket-aligned, `tag_bit_offset = 0`, gets `ceil(tag_bit_count / 64)` consecutive dedicated buckets. Cannot share.

Per-rule fields assigned:

```c
struct ScopedRule {
  // ...
  int32_t tag_bit_index;   // starting bucket index in bits[]
  int32_t tag_bit_offset;  // bit offset within starting bucket (0..63)
  int32_t tag_bit_count;
};
```

### `PEG_MEMOIZE_SHARED`

Rules that cannot co-exist at the same matching position share a slot and tag-bit region. Extra bits in the segment denote which rule the slot currently represents.

#### Exclusiveness analysis

Compute per rule R:
- `first_set(R)` — leaf token ids (expanding calls), excluding epsilon.
- `last_set(R)` — same, for the last token.
- `nullable(R)` — can match 0 tokens.
- `min_size(R)`, `max_size(R)` — token counts, `UINT32_MAX` for unlimited.

Rules may recursively call each other; use fix-point iteration.

Two rules A, B are **exclusive** iff at least one holds:
- `min_size(A) > 0 && min_size(B) > 0 && first_set(A) ∩ first_set(B) = ∅`
  (covers the `all_set` intersection case)
- `min_size(A) == max_size(A) == min_size(B) == max_size(B) > 0 && last_set(A) ∩ last_set(B) = ∅`
  (`first_set` already checked above)

#### Interference graph and coloring

Vertices = rules in the closure. Edge (A, B) iff A and B are **not** exclusive.

Hand to [coloring](coloring.md): it runs kissat (DSatur fallback on Windows), applies symmetry breaking via max-clique, and returns segmented color groups with each segment ≤ 64 vertices so a segment's bits fit in one `uint64_t`.

#### Slot encoding (negated-bitset)

Initial state: all bits and slots are `1` / `-1` (set by `tt_alloc_memoize_table()`).

Per rule, we know:
- `segment_bits = bits[segment_index] & segment_mask`
- `rule_bit = segment_bits & rule_bit_mask`
  - if `1`: may match; check slot. If slot != `-1`, cached match. Else parse:
    - on match: clear all other bits in the segment (exclusive), set slot.
    - on fail: clear the rule bit.
  - if `0`: cached failure.

#### Logical allocation

`_alloc_slot_bits()` and `_alloc_shared_tag_bits()` record what each segment needs; they do **not** pick physical bucket indices.

```c
typedef struct {
  int32_t color;           // graph coloring color id
  int32_t slot_bit_count;  // bits needed for slot encoding (= group size)
  int32_t tag_bit_count;   // max tag bits among rules in this segment
} SegmentAlloc;
```

- `_alloc_slot_bits()`: one `SegmentAlloc` per color group; `slot_bit_count = group size`.
- `_alloc_shared_tag_bits()`: `tag_bit_count = max(symtab_count(tags))` over rules in the group.

Per-rule fields assigned during logical allocation:

```c
struct ScopedRule {
  // ...
  int32_t tag_bit_offset;  // bit offset within tag region of the segment
  int32_t tag_bit_count;

  bool nullable;
  Bitset first_set; // excluding epsilon
  Bitset last_set;  // excluding epsilon

  int32_t segment_color;   // logical color group id
  uint64_t segment_mask;   // full mask of all rule bits in this segment
  uint64_t rule_bit_mask;  // single bit in segment_mask
};
```

#### Physical layout pass (`_finalize_layout`)

Per `SegmentAlloc`, compute buckets:
- `slot_buckets = 1` (slot bits always fit in one bucket; segments are ≤ 64 by coloring).
- `tag_buckets`:
  - `tag_bit_count == 0` → 0
  - `tag_bit_count <= 64 - slot_bit_count` → 0 (packs into slot-bucket gap)
  - else → `ceil(tag_bit_count / 64)` dedicated tag buckets.

Slot buckets of different segments are **greedy-bin-packed** into shared `uint64_t` buckets (place each segment into the first bucket with enough remaining bits; open a new bucket only when none fits). `rule_bit_mask` and `segment_mask` are shifted into the segment's bit-range within its shared bucket so that `bits[segment_index] & segment_mask` still isolates just this segment's slot bits.

Dedicated tag buckets (big-tag segments) are appended after the packed slot buckets and are not shared.

Fill physical indices:

```c
struct ScopedRule {
  // ... (in addition to fields above)
  int32_t tag_bit_index;   // physical starting bucket index in bits[]
  uint64_t segment_index;  // bits[segment_index]
  uint64_t slot_index;     // slots[slot_index], shared within segment
};
```

Tag bit placement:
- **Packed** (fits in slot bucket gap): `tag_bit_index = segment_index`, `tag_bit_offset = <segment's bit offset in shared bucket> + slot_bit_count`.
- **Dedicated** (own buckets): `tag_bit_index` points past all packed slot buckets, `tag_bit_offset = 0`.

#### Packing requirement

Slot bits and tag bits **must** share buckets whenever they fit: within a segment, when `tag_bit_count <= 64 - slot_bit_count`, tag bits live in the same `uint64_t` as the slot bits. Across segments, slot buckets are greedy-bin-packed as described above. Dedicated tag buckets are only used when tags genuinely overflow the slot-bucket gap.

The same packing discipline applies to naive-mode tag bit allocation: small rules greedy-knapsack into shared buckets; a rule only consumes a new bucket when no existing bucket has room.

Across a closure, the resulting `bits[]` must not contain large unallocated gaps. Concretely, for every pair of buckets `(i, j)` in `bits[0 .. bits_bucket_size)`, it is a bug if `used(i) + used(j) <= 64` (both could have been merged) — except when one of them is a dedicated tag bucket of a big rule (dedicated regions are never merged with slot buckets, by design).

Final layout:

```c
struct Col${scope_name} {
  uint64_t bits[{total_buckets}];
  int32_t slots[{(segment_count + 1) / 2 * 2}]; // 64-bit aligned
};
```

Set `closure->bits_bucket_size = total_buckets` and `closure->slots_size = segment_count`.

In shared mode, failures are recorded by clearing the rule bit; `-2` is not used in slots.

## Dependencies

- [coloring](coloring.md) — k-coloring with segment-of-64 output.
- bitset, darray, symtab — utility only.

Must **not** depend on parse, peg_ir, peg_gen, peg_analyze's IR/codegen concerns, or token_tree runtime.

## Acceptance criteria

- Given a hand-built `ScopeClosure` with `scoped_rules` and `tags` populated, calling `peg_alloc_scope(closure, mode)` fills all allocation fields and sets `bits_bucket_size` / `slots_size` consistently with the layout rules above.
- For naive mode: tag buckets follow the small/big knapsack rule; each rule has its own slot.
- For shared mode: rules in the same color group share `segment_index` / `slot_index`; their `rule_bit_mask` values are pairwise disjoint and together equal `segment_mask`; tag placement obeys packed-vs-dedicated rule.
- **Packing test**: construct closures where slot bits and tag bits together fit in one bucket, and assert `bits_bucket_size == segment_count` (no dedicated tag buckets emitted). Construct closures with varying tag counts and assert no bucket is left with a large unallocated gap while another bucket has room — i.e. the greedy knapsack invariant above holds.
- Reuses spec names verbatim — no re-invented identifiers.
