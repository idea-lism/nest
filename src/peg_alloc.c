// specs/peg_alloc.md
#include "bitset.h"
#include "coloring.h"
#include "darray.h"
#include "graph.h"
#include "peg.h"
#include "symtab.h"
#include "xmalloc.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// ============================================================
// Naive-mode tag bit packing
// ============================================================

typedef struct {
  int32_t rule_idx;
  int32_t tag_size;
} _TagEntry;

static int _cmp_tag_entry_desc(const void* a, const void* b) {
  int32_t ta = ((const _TagEntry*)a)->tag_size;
  int32_t tb = ((const _TagEntry*)b)->tag_size;
  if (ta != tb) {
    return (tb > ta) ? 1 : -1;
  }
  return ((const _TagEntry*)a)->rule_idx - ((const _TagEntry*)b)->rule_idx;
}

static void _alloc_tag_bits(ScopeClosure* cl) {
  _TagEntry* entries = NULL;
  int32_t entry_size = 0;
  _TagEntry* big_entries = NULL;
  int32_t big_size = 0;
  for (size_t i = 0; i < darray_size(cl->scoped_rules); i++) {
    int32_t nt = symtab_count(&cl->scoped_rules[i].tags);
    if (nt > 64) {
      big_entries = XREALLOC(big_entries, (size_t)(big_size + 1) * sizeof(_TagEntry));
      big_entries[big_size++] = (_TagEntry){.rule_idx = (int32_t)i, .tag_size = nt};
    } else if (nt > 0) {
      entries = XREALLOC(entries, (size_t)(entry_size + 1) * sizeof(_TagEntry));
      entries[entry_size++] = (_TagEntry){.rule_idx = (int32_t)i, .tag_size = nt};
    } else {
      cl->scoped_rules[i].tag_bit_index = 0;
      cl->scoped_rules[i].tag_bit_offset = 0;
      cl->scoped_rules[i].tag_bit_count = 0;
    }
  }

  if (entry_size == 0 && big_size == 0) {
    cl->bits_bucket_size = 0;
    XFREE(entries);
    XFREE(big_entries);
    return;
  }

  // Small rules: knapsack-pack into shared buckets
  qsort(entries, (size_t)entry_size, sizeof(_TagEntry), _cmp_tag_entry_desc);

  int32_t bucket_size = 0;
  int32_t* bucket_remaining = NULL;

  for (int32_t e = 0; e < entry_size; e++) {
    int32_t nt = entries[e].tag_size;
    int32_t best = -1;
    for (int32_t b = 0; b < bucket_size; b++) {
      if (bucket_remaining[b] >= nt) {
        best = b;
        break;
      }
    }
    if (best < 0) {
      bucket_remaining = XREALLOC(bucket_remaining, (size_t)(bucket_size + 1) * sizeof(int32_t));
      bucket_remaining[bucket_size] = 64;
      best = bucket_size;
      bucket_size++;
    }
    int32_t offset = 64 - bucket_remaining[best];
    ScopedRule* sr = &cl->scoped_rules[entries[e].rule_idx];
    sr->tag_bit_index = best;
    sr->tag_bit_offset = offset;
    sr->tag_bit_count = nt;
    bucket_remaining[best] -= nt;
  }

  // Big rules: bucket-aligned, consecutive dedicated buckets
  for (int32_t e = 0; e < big_size; e++) {
    int32_t nt = big_entries[e].tag_size;
    int32_t needed = (nt + 63) / 64;
    ScopedRule* sr = &cl->scoped_rules[big_entries[e].rule_idx];
    sr->tag_bit_index = bucket_size;
    sr->tag_bit_offset = 0;
    sr->tag_bit_count = nt;
    bucket_size += needed;
  }

  cl->bits_bucket_size = bucket_size;
  XFREE(entries);
  XFREE(big_entries);
  XFREE(bucket_remaining);
}

// ============================================================
// Naive slot allocation: one slot per rule
// ============================================================

static void _alloc_naive_slots(ScopeClosure* cl) {
  int32_t n = (int32_t)darray_size(cl->scoped_rules);
  for (int32_t i = 0; i < n; i++) {
    cl->scoped_rules[i].slot_index = (uint64_t)i;
  }
  cl->slots_size = n;
}

// ============================================================
// Analysis: nullable, min/max size, first/last sets
// ============================================================

static void _compute_nullable(ScopeClosure* cl, ScopedRule* sr, ScopedUnit* unit) {
  switch (unit->kind) {
  case SCOPED_UNIT_TERM:
    sr->nullable = false;
    sr->min_size = 1;
    sr->max_size = 1;
    break;
  case SCOPED_UNIT_CALL: {
    int32_t cid = symtab_find(&cl->scoped_rule_names, unit->as.callee);
    if (cid >= 0) {
      int32_t idx = cid - cl->scoped_rule_names.start_num;
      sr->nullable = cl->scoped_rules[idx].nullable;
      sr->min_size = cl->scoped_rules[idx].min_size;
      sr->max_size = cl->scoped_rules[idx].max_size;
    }
    break;
  }
  case SCOPED_UNIT_SEQ: {
    sr->nullable = true;
    sr->min_size = 0;
    sr->max_size = 0;
    for (size_t i = 0; i < darray_size(unit->as.children); i++) {
      ScopedRule tmp = {0};
      _compute_nullable(cl, &tmp, &unit->as.children[i]);
      if (!tmp.nullable) {
        sr->nullable = false;
      }
      sr->min_size += tmp.min_size;
      if (sr->max_size != UINT32_MAX && tmp.max_size != UINT32_MAX) {
        sr->max_size += tmp.max_size;
      } else {
        sr->max_size = UINT32_MAX;
      }
    }
    break;
  }
  case SCOPED_UNIT_BRANCHES: {
    sr->nullable = false;
    sr->min_size = UINT32_MAX;
    sr->max_size = 0;
    for (size_t i = 0; i < darray_size(unit->as.children); i++) {
      ScopedRule tmp = {0};
      _compute_nullable(cl, &tmp, &unit->as.children[i]);
      if (tmp.nullable) {
        sr->nullable = true;
      }
      if (tmp.min_size < sr->min_size) {
        sr->min_size = tmp.min_size;
      }
      if (tmp.max_size == UINT32_MAX || sr->max_size == UINT32_MAX) {
        sr->max_size = UINT32_MAX;
      } else if (tmp.max_size > sr->max_size) {
        sr->max_size = tmp.max_size;
      }
    }
    break;
  }
  case SCOPED_UNIT_MAYBE: {
    ScopedRule tmp = {0};
    _compute_nullable(cl, &tmp, unit->as.base);
    sr->nullable = true;
    sr->min_size = 0;
    sr->max_size = tmp.max_size;
    break;
  }
  case SCOPED_UNIT_STAR: {
    sr->nullable = true;
    sr->min_size = 0;
    sr->max_size = UINT32_MAX;
    break;
  }
  case SCOPED_UNIT_PLUS: {
    ScopedRule tmp = {0};
    _compute_nullable(cl, &tmp, unit->as.interlace.lhs);
    sr->nullable = tmp.nullable;
    sr->min_size = tmp.min_size;
    sr->max_size = UINT32_MAX;
    break;
  }
  }
}

static bool _is_unit_nullable(ScopeClosure* cl, ScopedUnit* unit) {
  switch (unit->kind) {
  case SCOPED_UNIT_TERM:
    return false;
  case SCOPED_UNIT_CALL: {
    int32_t cid = symtab_find(&cl->scoped_rule_names, unit->as.callee);
    if (cid >= 0) {
      return cl->scoped_rules[cid - cl->scoped_rule_names.start_num].nullable;
    }
    return false;
  }
  case SCOPED_UNIT_SEQ:
    for (size_t i = 0; i < darray_size(unit->as.children); i++) {
      if (!_is_unit_nullable(cl, &unit->as.children[i])) {
        return false;
      }
    }
    return true;
  case SCOPED_UNIT_BRANCHES:
    for (size_t i = 0; i < darray_size(unit->as.children); i++) {
      if (_is_unit_nullable(cl, &unit->as.children[i])) {
        return true;
      }
    }
    return false;
  case SCOPED_UNIT_MAYBE:
  case SCOPED_UNIT_STAR:
    return true;
  case SCOPED_UNIT_PLUS:
    return _is_unit_nullable(cl, unit->as.interlace.lhs);
  }
  return false;
}

typedef enum { SET_FIRST, SET_LAST } _SetDir;

static void _compute_set(ScopeClosure* cl, ScopedUnit* unit, Bitset* set, _SetDir dir) {
  switch (unit->kind) {
  case SCOPED_UNIT_TERM:
    bitset_add_bit(set, (uint32_t)unit->as.term_id);
    break;
  case SCOPED_UNIT_CALL: {
    int32_t cid = symtab_find(&cl->scoped_rule_names, unit->as.callee);
    if (cid >= 0) {
      int32_t idx = cid - cl->scoped_rule_names.start_num;
      Bitset* src = (dir == SET_FIRST) ? cl->scoped_rules[idx].first_set : cl->scoped_rules[idx].last_set;
      if (src) {
        bitset_or_into(set, src);
      }
    }
    break;
  }
  case SCOPED_UNIT_SEQ: {
    int32_t n = (int32_t)darray_size(unit->as.children);
    int32_t start = (dir == SET_FIRST) ? 0 : n - 1;
    int32_t step = (dir == SET_FIRST) ? 1 : -1;
    for (int32_t i = start; i >= 0 && i < n; i += step) {
      _compute_set(cl, &unit->as.children[i], set, dir);
      if (!_is_unit_nullable(cl, &unit->as.children[i])) {
        break;
      }
    }
    break;
  }
  case SCOPED_UNIT_BRANCHES:
    for (size_t i = 0; i < darray_size(unit->as.children); i++) {
      _compute_set(cl, &unit->as.children[i], set, dir);
    }
    break;
  case SCOPED_UNIT_MAYBE:
    _compute_set(cl, unit->as.base, set, dir);
    break;
  case SCOPED_UNIT_STAR:
  case SCOPED_UNIT_PLUS:
    _compute_set(cl, unit->as.interlace.lhs, set, dir);
    break;
  }
}

static void _stamp_unit_nullable(ScopeClosure* cl, ScopedUnit* unit) {
  unit->nullable = _is_unit_nullable(cl, unit);
  switch (unit->kind) {
  case SCOPED_UNIT_SEQ:
  case SCOPED_UNIT_BRANCHES:
    for (size_t i = 0; i < darray_size(unit->as.children); i++) {
      _stamp_unit_nullable(cl, &unit->as.children[i]);
    }
    break;
  case SCOPED_UNIT_MAYBE:
    _stamp_unit_nullable(cl, unit->as.base);
    break;
  case SCOPED_UNIT_STAR:
  case SCOPED_UNIT_PLUS:
    _stamp_unit_nullable(cl, unit->as.interlace.lhs);
    if (unit->as.interlace.rhs) {
      _stamp_unit_nullable(cl, unit->as.interlace.rhs);
    }
    break;
  default:
    break;
  }
}

static void _analyze_rules(ScopeClosure* cl) {
  int32_t n = (int32_t)darray_size(cl->scoped_rules);

  for (int32_t i = 0; i < n; i++) {
    cl->scoped_rules[i].nullable = false;
    cl->scoped_rules[i].min_size = 0;
    cl->scoped_rules[i].max_size = 0;
  }
  for (;;) {
    bool changed = false;
    for (int32_t i = 0; i < n; i++) {
      bool old_nullable = cl->scoped_rules[i].nullable;
      uint32_t old_min = cl->scoped_rules[i].min_size;
      uint32_t old_max = cl->scoped_rules[i].max_size;
      cl->scoped_rules[i].nullable = false;
      cl->scoped_rules[i].min_size = 0;
      cl->scoped_rules[i].max_size = 0;
      _compute_nullable(cl, &cl->scoped_rules[i], &cl->scoped_rules[i].body);
      if (cl->scoped_rules[i].nullable != old_nullable || cl->scoped_rules[i].min_size != old_min ||
          cl->scoped_rules[i].max_size != old_max) {
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
  }

  for (int32_t i = 0; i < n; i++) {
    cl->scoped_rules[i].first_set = bitset_new();
    cl->scoped_rules[i].last_set = bitset_new();
  }
  for (;;) {
    bool changed = false;
    for (int32_t i = 0; i < n; i++) {
      Bitset* old_first = cl->scoped_rules[i].first_set;
      Bitset* old_last = cl->scoped_rules[i].last_set;
      cl->scoped_rules[i].first_set = bitset_new();
      cl->scoped_rules[i].last_set = bitset_new();
      _compute_set(cl, &cl->scoped_rules[i].body, cl->scoped_rules[i].first_set, SET_FIRST);
      _compute_set(cl, &cl->scoped_rules[i].body, cl->scoped_rules[i].last_set, SET_LAST);
      if (!bitset_equal(cl->scoped_rules[i].first_set, old_first) ||
          !bitset_equal(cl->scoped_rules[i].last_set, old_last)) {
        changed = true;
      }
      bitset_del(old_first);
      bitset_del(old_last);
    }
    if (!changed) {
      break;
    }
  }

  for (int32_t i = 0; i < n; i++) {
    _stamp_unit_nullable(cl, &cl->scoped_rules[i].body);
  }
}

// ============================================================
// Shared mode: exclusiveness, graph coloring, tag bit gap-filling
// ============================================================

static bool _are_exclusive(ScopedRule* a, ScopedRule* b) {
  if (a->min_size > 0 && b->min_size > 0) {
    Bitset* inter = bitset_and(a->first_set, b->first_set);
    bool disjoint = (bitset_size(inter) == 0);
    bitset_del(inter);
    if (disjoint) {
      return true;
    }
  }
  if (a->min_size > 0 && a->max_size != UINT32_MAX && a->min_size == a->max_size && b->max_size != UINT32_MAX &&
      b->min_size == b->max_size && a->min_size == b->min_size) {
    Bitset* inter = bitset_and(a->last_set, b->last_set);
    bool disjoint = (bitset_size(inter) == 0);
    bitset_del(inter);
    if (disjoint) {
      return true;
    }
  }
  return false;
}

static void _alloc_slot_bits(ScopeClosure* cl) {
  int32_t n = (int32_t)darray_size(cl->scoped_rules);
  if (n == 0) {
    cl->slots_size = 0;
    return;
  }
  Graph* g = graph_new(n);
  for (int32_t i = 0; i < n; i++) {
    for (int32_t j = i + 1; j < n; j++) {
      if (!_are_exclusive(&cl->scoped_rules[i], &cl->scoped_rules[j])) {
        graph_add_edge(g, i, j);
      }
    }
  }
  int32_t* edges = graph_edges(g);
  int32_t edge_size = graph_n_edges(g);
  ColoringResult* cr = NULL;
  for (int32_t k = 1; k <= n; k++) {
    cr = coloring_solve(n, edges, edge_size, k, 10000, 42);
    if (cr) {
      break;
    }
  }
  graph_del(g);
  assert(cr);
  cl->slots_size = coloring_get_sg_size(cr);
  for (int32_t i = 0; i < n; i++) {
    int32_t sg_id;
    int64_t seg_mask;
    coloring_get_segment_info(cr, i, &sg_id, &seg_mask);
    cl->scoped_rules[i].segment_color = sg_id;
    cl->scoped_rules[i].rule_bit_mask = (uint64_t)seg_mask;
  }
  // compute segment_mask (union of all rule_bit_masks in same color group)
  for (int32_t i = 0; i < n; i++) {
    uint64_t seg_union = 0;
    int32_t color = cl->scoped_rules[i].segment_color;
    for (int32_t j = 0; j < n; j++) {
      if (cl->scoped_rules[j].segment_color == color) {
        seg_union |= cl->scoped_rules[j].rule_bit_mask;
      }
    }
    cl->scoped_rules[i].segment_mask = seg_union;
  }
  coloring_result_del(cr);
}

// Physical layout pass: assign segment_index, slot_index, tag_bit_index, tag_bit_offset
static void _finalize_layout(ScopeClosure* cl, int32_t* seg_max_tags) {
  int32_t rule_size = (int32_t)darray_size(cl->scoped_rules);
  int32_t segment_size = (int32_t)cl->slots_size;

  // Compute slot_bit_count per segment (popcount of segment_mask)
  int32_t* seg_slot_bits = XCALLOC((size_t)segment_size, sizeof(int32_t));
  for (int32_t i = 0; i < rule_size; i++) {
    int32_t seg = cl->scoped_rules[i].segment_color;
    if (seg >= 0 && seg < segment_size && seg_slot_bits[seg] == 0) {
      seg_slot_bits[seg] = (int32_t)__builtin_popcountll(cl->scoped_rules[i].segment_mask);
    }
  }

  // Layout each segment contiguously: slot_bucket + optional tag_buckets
  int32_t total_buckets = 0;
  int32_t* seg_phys_index = XCALLOC((size_t)segment_size, sizeof(int32_t));
  int32_t* seg_tag_index = XCALLOC((size_t)segment_size, sizeof(int32_t));
  int32_t* seg_tag_offset = XCALLOC((size_t)segment_size, sizeof(int32_t));

  for (int32_t seg = 0; seg < segment_size; seg++) {
    seg_phys_index[seg] = total_buckets;
    total_buckets++; // slot bucket

    int32_t max_tags = seg_max_tags[seg];
    int32_t slot_bits = seg_slot_bits[seg];
    if (max_tags == 0) {
      seg_tag_index[seg] = seg_phys_index[seg];
      seg_tag_offset[seg] = 0;
    } else if (max_tags <= 64 - slot_bits) {
      // Packed: tag bits fit in slot bucket gap
      seg_tag_index[seg] = seg_phys_index[seg];
      seg_tag_offset[seg] = slot_bits;
    } else {
      // Dedicated: tag bits need own buckets
      seg_tag_index[seg] = total_buckets;
      seg_tag_offset[seg] = 0;
      int32_t needed = (max_tags + 63) / 64;
      total_buckets += needed;
    }
  }

  // Fill physical indices into each ScopedRule
  for (int32_t i = 0; i < rule_size; i++) {
    ScopedRule* sr = &cl->scoped_rules[i];
    int32_t seg = sr->segment_color;
    if (seg >= 0 && seg < segment_size) {
      sr->segment_index = (uint64_t)seg_phys_index[seg];
      sr->slot_index = (uint64_t)seg; // slot index = logical segment
      if (sr->tag_bit_count > 0) {
        sr->tag_bit_index = seg_tag_index[seg];
        sr->tag_bit_offset = seg_tag_offset[seg];
      }
    }
  }

  cl->bits_bucket_size = total_buckets;

  XFREE(seg_slot_bits);
  XFREE(seg_phys_index);
  XFREE(seg_tag_index);
  XFREE(seg_tag_offset);
}

static void _alloc_shared_tag_bits(ScopeClosure* cl) {
  int32_t rule_size = (int32_t)darray_size(cl->scoped_rules);
  int32_t segment_size = (int32_t)cl->slots_size;

  int32_t* seg_max_tags = XCALLOC((size_t)segment_size, sizeof(int32_t));
  for (int32_t i = 0; i < rule_size; i++) {
    int32_t seg = cl->scoped_rules[i].segment_color;
    if (seg >= 0 && seg < segment_size) {
      int32_t nt = symtab_count(&cl->scoped_rules[i].tags);
      if (nt > seg_max_tags[seg]) {
        seg_max_tags[seg] = nt;
      }
    }
  }

  // Assign tag_bit_offset and tag_bit_count per rule (logical, no physical index yet)
  for (int32_t i = 0; i < rule_size; i++) {
    int32_t nt = symtab_count(&cl->scoped_rules[i].tags);
    cl->scoped_rules[i].tag_bit_count = nt;
    // tag_bit_offset is filled by _finalize_layout
    cl->scoped_rules[i].tag_bit_offset = 0;
    cl->scoped_rules[i].tag_bit_index = 0;
  }

  _finalize_layout(cl, seg_max_tags);

  XFREE(seg_max_tags);
}

// ============================================================
// Public API
// ============================================================

void peg_alloc_scope(ScopeClosure* closure, MemoizeMode mode) {
  _analyze_rules(closure);
  if (mode == MEMOIZE_SHARED) {
    _alloc_slot_bits(closure);
    _alloc_shared_tag_bits(closure);
  } else {
    _alloc_naive_slots(closure);
    _alloc_tag_bits(closure);
  }
}
