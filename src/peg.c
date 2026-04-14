// specs/peg.md
#include "peg.h"
#include "bitset.h"
#include "coloring.h"
#include "darray.h"
#include "graph.h"
#include "peg_ir.h"
#include "symtab.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================
// Helpers
// ============================================================

__attribute__((format(printf, 1, 2))) static char* _fmt(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char* buf = malloc((size_t)n + 1);
  va_start(ap, fmt);
  vsnprintf(buf, (size_t)n + 1, fmt, ap);
  va_end(ap);
  return buf;
}

static uint64_t _tag_mask(int32_t n_bits, uint64_t offset) {
  if (n_bits <= 0) {
    return 0;
  }
  if (n_bits >= 64) {
    return UINT64_MAX << offset;
  }
  return ((1ULL << n_bits) - 1) << offset;
}

// ============================================================
// O(1) rule lookup by global_id
// ============================================================

// Built once at peg_gen entry. rule_by_gid[gid - start_num] -> PegRule* (or NULL).
typedef struct {
  PegGenInput* input;
  PegRule** rule_by_gid; // indexed by (global_id - rule_names.start_num)
  int32_t rule_count;    // symtab_count(rule_names)
} RuleLookup;

static bool _rule_gid_index(RuleLookup* lu, int32_t global_id, int32_t* out_idx) {
  int32_t idx = global_id - lu->input->rule_names.start_num;
  if (idx >= 0 && idx < lu->rule_count) {
    *out_idx = idx;
    return true;
  }
  return false;
}

static RuleLookup _build_rule_lookup(PegGenInput* input) {
  RuleLookup lu = {.input = input};
  lu.rule_count = symtab_count(&input->rule_names);
  lu.rule_by_gid = calloc((size_t)lu.rule_count, sizeof(PegRule*));
  for (size_t i = 0; i < darray_size(input->rules); i++) {
    int32_t idx;
    if (_rule_gid_index(&lu, input->rules[i].global_id, &idx)) {
      lu.rule_by_gid[idx] = &input->rules[i];
    }
  }
  return lu;
}

static PegRule* _lookup_rule(RuleLookup* lu, int32_t global_id) {
  int32_t idx;
  if (_rule_gid_index(lu, global_id, &idx)) {
    return lu->rule_by_gid[idx];
  }
  return NULL;
}

static void _free_rule_lookup(RuleLookup* lu) { free(lu->rule_by_gid); }

static bool _is_scope_term(PegGenInput* input, int32_t id) {
  return id < symtab_count(&input->scope_names) + input->scope_names.start_num;
}

static const char* _term_name(PegGenInput* input, int32_t id) {
  if (_is_scope_term(input, id)) {
    return symtab_get(&input->scope_names, id);
  }
  return symtab_get(&input->tokens, id);
}

// ============================================================
// Scope closure gathering and breakdown
// ============================================================

typedef struct {
  RuleLookup* lu;
  ScopeClosure* closure;
  int32_t multiplier_num;
} GatherCtx;

// Ensure scoped_rules array covers a symtab ID. Returns index.
static int32_t _intern_rule_id(ScopeClosure* cl, int32_t id, int32_t original_global_id) {
  int32_t idx = id - cl->scoped_rule_names.start_num;
  while ((int32_t)darray_size(cl->scoped_rules) <= idx) {
    ScopedRule empty = {0};
    empty.original_global_id = -1;
    darray_push(cl->scoped_rules, empty);
  }
  if (cl->scoped_rules[idx].original_global_id == -1 && original_global_id >= 0) {
    cl->scoped_rules[idx].original_global_id = original_global_id;
  }
  return idx;
}

static int32_t _intern_rule(ScopeClosure* cl, const char* scope_name, const char* rule_name,
                            int32_t original_global_id) {
  int32_t id = symtab_intern_f(&cl->scoped_rule_names, "%s$%s", scope_name, rule_name);
  return _intern_rule_id(cl, id, original_global_id);
}

static ScopedUnit _breakdown(GatherCtx* gctx, PegUnit* unit, const char* parent_rule_name);

static ScopedUnit _breakdown_single(GatherCtx* gctx, PegUnit* unit) {
  ScopedUnit su = {.tag_bit_local_offset = -1};
  if (unit->kind == PEG_TERM) {
    su.kind = SCOPED_UNIT_TERM;
    su.as.term_id = unit->id;
    return su;
  }
  if (unit->kind == PEG_CALL) {
    PegRule* callee = _lookup_rule(gctx->lu, unit->id);
    if (!callee || callee->scope_id >= 0) {
      su.kind = SCOPED_UNIT_TERM;
      su.as.term_id = unit->id;
      return su;
    }
    const char* callee_name = symtab_get(&gctx->lu->input->rule_names, unit->id);
    int32_t idx = _intern_rule(gctx->closure, gctx->closure->scope_name, callee_name, unit->id);
    if (!gctx->closure->scoped_rules[idx].scoped_rule_name) {
      gctx->closure->scoped_rules[idx].scoped_rule_name = (const char*)1; // processing sentinel
      ScopedUnit body = _breakdown(gctx, &callee->body, callee_name);
      gctx->closure->scoped_rules[idx].body = body;
    }
    su.kind = SCOPED_UNIT_CALL;
    su.as.callee = (const char*)(intptr_t)(idx + gctx->closure->scoped_rule_names.start_num);
    return su;
  }
  su.kind = SCOPED_UNIT_TERM;
  su.as.term_id = 0;
  return su;
}

static ScopedUnit _breakdown(GatherCtx* gctx, PegUnit* unit, const char* parent_rule_name) {
  ScopedUnit su = {.tag_bit_local_offset = -1};

  // handle multiplier: create child rule
  if (unit->multiplier) {
    gctx->multiplier_num++;
    int32_t child_id = symtab_intern_f(&gctx->closure->scoped_rule_names, "%s$%s$%d", gctx->closure->scope_name,
                                       parent_rule_name, gctx->multiplier_num);
    int32_t child_idx = _intern_rule_id(gctx->closure, child_id, -1);

    PegUnit lhs_copy = *unit;
    lhs_copy.multiplier = 0;
    lhs_copy.interlace_rhs_kind = 0;
    lhs_copy.interlace_rhs_id = 0;
    ScopedUnit lhs = _breakdown(gctx, &lhs_copy, parent_rule_name);
    ScopedUnit* lhs_heap = malloc(sizeof(ScopedUnit));
    *lhs_heap = lhs;

    ScopedUnit* rhs_heap = NULL;
    if (unit->interlace_rhs_kind) {
      PegUnit rhs_unit = {.kind = unit->interlace_rhs_kind, .id = unit->interlace_rhs_id};
      ScopedUnit rhs = _breakdown_single(gctx, &rhs_unit);
      rhs_heap = malloc(sizeof(ScopedUnit));
      *rhs_heap = rhs;
    }

    ScopedUnit body = {.tag_bit_local_offset = -1};
    switch (unit->multiplier) {
    case '?':
      body.kind = SCOPED_UNIT_MAYBE;
      body.as.base = lhs_heap;
      break;
    case '*':
      body.kind = SCOPED_UNIT_STAR;
      body.as.interlace = (ScopedInterlace){lhs_heap, rhs_heap};
      break;
    case '+':
      body.kind = SCOPED_UNIT_PLUS;
      body.as.interlace = (ScopedInterlace){lhs_heap, rhs_heap};
      break;
    default:
      break;
    }

    if (!gctx->closure->scoped_rules[child_idx].scoped_rule_name) {
      gctx->closure->scoped_rules[child_idx].scoped_rule_name = (const char*)1;
    }
    gctx->closure->scoped_rules[child_idx].body = body;

    su.kind = SCOPED_UNIT_CALL;
    su.as.callee = (const char*)(intptr_t)(child_idx + gctx->closure->scoped_rule_names.start_num);
    return su;
  }

  if (unit->kind == PEG_TERM || unit->kind == PEG_CALL) {
    return _breakdown_single(gctx, unit);
  }

  if (unit->kind == PEG_SEQ) {
    int32_t n = (int32_t)darray_size(unit->children);
    if (n == 1) {
      ScopedUnit child = _breakdown(gctx, &unit->children[0], parent_rule_name);
      if (unit->children[0].tag && unit->children[0].tag[0]) {
        child.tag_bit_local_offset = 0;
      }
      return child;
    }
    su.kind = SCOPED_UNIT_SEQ;
    su.as.children = darray_new(sizeof(ScopedUnit), 0);
    for (int32_t i = 0; i < n; i++) {
      ScopedUnit child = _breakdown(gctx, &unit->children[i], parent_rule_name);
      darray_push(su.as.children, child);
    }
    return su;
  }

  if (unit->kind == PEG_BRANCHES) {
    su.kind = SCOPED_UNIT_BRANCHES;
    su.as.children = darray_new(sizeof(ScopedUnit), 0);
    for (size_t i = 0; i < darray_size(unit->children); i++) {
      ScopedUnit child = _breakdown(gctx, &unit->children[i], parent_rule_name);
      if (child.kind == SCOPED_UNIT_BRANCHES) {
        // wrap nested branches in a seq child rule (spec: don't inline branches)
        gctx->multiplier_num++;
        int32_t wrap_id = symtab_intern_f(&gctx->closure->scoped_rule_names, "%s$%s$%d", gctx->closure->scope_name,
                                          parent_rule_name, gctx->multiplier_num);
        int32_t wrap_idx = _intern_rule_id(gctx->closure, wrap_id, -1);
        if (!gctx->closure->scoped_rules[wrap_idx].scoped_rule_name) {
          gctx->closure->scoped_rules[wrap_idx].scoped_rule_name = (const char*)1;
        }
        ScopedUnit seq_wrap = {.kind = SCOPED_UNIT_SEQ, .tag_bit_local_offset = -1};
        seq_wrap.as.children = darray_new(sizeof(ScopedUnit), 0);
        darray_push(seq_wrap.as.children, child);
        gctx->closure->scoped_rules[wrap_idx].body = seq_wrap;

        ScopedUnit call = {.kind = SCOPED_UNIT_CALL, .tag_bit_local_offset = -1};
        call.as.callee = (const char*)(intptr_t)(wrap_idx + gctx->closure->scoped_rule_names.start_num);
        if (unit->children[i].tag && unit->children[i].tag[0]) {
          call.tag_bit_local_offset = 0;
        }
        darray_push(su.as.children, call);
      } else {
        if (unit->children[i].tag && unit->children[i].tag[0]) {
          child.tag_bit_local_offset = 0;
        }
        darray_push(su.as.children, child);
      }
    }
    return su;
  }

  return su;
}

// Resolve symtab-id-as-intptr callees to actual const char* pointers.
static void _resolve_unit_names(ScopeClosure* cl, ScopedUnit* su) {
  switch (su->kind) {
  case SCOPED_UNIT_CALL:
    su->as.callee = symtab_get(&cl->scoped_rule_names, (int32_t)(intptr_t)su->as.callee);
    break;
  case SCOPED_UNIT_SEQ:
  case SCOPED_UNIT_BRANCHES:
    for (size_t i = 0; i < darray_size(su->as.children); i++) {
      _resolve_unit_names(cl, &su->as.children[i]);
    }
    break;
  case SCOPED_UNIT_MAYBE:
    _resolve_unit_names(cl, su->as.base);
    break;
  case SCOPED_UNIT_STAR:
  case SCOPED_UNIT_PLUS:
    _resolve_unit_names(cl, su->as.interlace.lhs);
    if (su->as.interlace.rhs) {
      _resolve_unit_names(cl, su->as.interlace.rhs);
    }
    break;
  default:
    break;
  }
}

static ScopeClosure _gather_scope_closures(RuleLookup* lu, PegRule* scope_rule) {
  ScopeClosure cl = {0};
  cl.scope_name = symtab_get(&lu->input->rule_names, scope_rule->global_id);
  cl.scope_id = scope_rule->scope_id;
  symtab_init(&cl.scoped_rule_names, 0);
  cl.scoped_rules = darray_new(sizeof(ScopedRule), 0);

  GatherCtx gctx = {.lu = lu, .closure = &cl, .multiplier_num = 0};

  int32_t scope_idx = _intern_rule(&cl, cl.scope_name, cl.scope_name, scope_rule->global_id);
  cl.scoped_rules[scope_idx].scoped_rule_name = (const char*)1;
  ScopedUnit scope_body = _breakdown(&gctx, &scope_rule->body, cl.scope_name);
  cl.scoped_rules[scope_idx].body = scope_body;

  // resolve name pointers (symtab is now stable)
  int32_t rule_size = (int32_t)darray_size(cl.scoped_rules);
  for (int32_t i = 0; i < rule_size; i++) {
    cl.scoped_rules[i].scoped_rule_name = symtab_get(&cl.scoped_rule_names, i + cl.scoped_rule_names.start_num);
    symtab_init(&cl.scoped_rules[i].tags, 0);
  }
  for (int32_t i = 0; i < rule_size; i++) {
    _resolve_unit_names(&cl, &cl.scoped_rules[i].body);
  }

  return cl;
}

// ============================================================
// Tag allocation
// ============================================================

static void _alloc_tags_unit(ScopedUnit* unit, Symtab* tags, PegUnit* orig) {
  if (!orig) {
    return;
  }
  // handle single-child seq inlining
  if (orig->kind == PEG_SEQ && unit->kind != SCOPED_UNIT_SEQ) {
    if (darray_size(orig->children) == 1) {
      _alloc_tags_unit(unit, tags, &orig->children[0]);
      return;
    }
  }
  if (unit->kind == SCOPED_UNIT_BRANCHES && orig->kind == PEG_BRANCHES) {
    for (size_t i = 0; i < darray_size(unit->as.children) && i < darray_size(orig->children); i++) {
      if (orig->children[i].tag && orig->children[i].tag[0]) {
        int32_t tag_id = symtab_intern(tags, orig->children[i].tag);
        unit->as.children[i].tag_bit_local_offset = tag_id - tags->start_num;
      }
    }
  }
  if (orig->kind == PEG_SEQ && unit->kind == SCOPED_UNIT_SEQ) {
    for (size_t i = 0; i < darray_size(unit->as.children) && i < darray_size(orig->children); i++) {
      _alloc_tags_unit(&unit->as.children[i], tags, &orig->children[i]);
    }
  }
}

static void _alloc_tags(RuleLookup* lu, ScopeClosure* cl) {
  for (size_t i = 0; i < darray_size(cl->scoped_rules); i++) {
    ScopedRule* sr = &cl->scoped_rules[i];
    if (sr->original_global_id < 0) {
      continue;
    }
    PegRule* orig = _lookup_rule(lu, sr->original_global_id);
    if (!orig) {
      continue;
    }
    _alloc_tags_unit(&sr->body, &sr->tags, &orig->body);
  }
}

// ============================================================
// Tag bit packing (greedy bin-packing into uint64_t buckets)
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
  // collect rules that have tags
  _TagEntry* entries = NULL;
  int32_t entry_size = 0;
  for (size_t i = 0; i < darray_size(cl->scoped_rules); i++) {
    int32_t nt = symtab_count(&cl->scoped_rules[i].tags);
    if (nt > 0) {
      entries = realloc(entries, (size_t)(entry_size + 1) * sizeof(_TagEntry));
      entries[entry_size++] = (_TagEntry){.rule_idx = i, .tag_size = nt};
    } else {
      cl->scoped_rules[i].tag_bit_index = 0;
      cl->scoped_rules[i].tag_bit_mask = 0;
      cl->scoped_rules[i].tag_bit_offset = 0;
    }
  }

  if (entry_size == 0) {
    cl->bits_bucket_size = 0;
    free(entries);
    return;
  }

  // sort by tag_size descending (first-fit-decreasing)
  qsort(entries, (size_t)entry_size, sizeof(_TagEntry), _cmp_tag_entry_desc);

  // greedy first-fit into 64-bit buckets
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
      bucket_remaining = realloc(bucket_remaining, (size_t)(bucket_size + 1) * sizeof(int32_t));
      bucket_remaining[bucket_size] = 64;
      best = bucket_size;
      bucket_size++;
    }
    int32_t offset = 64 - bucket_remaining[best];
    ScopedRule* sr = &cl->scoped_rules[entries[e].rule_idx];
    sr->tag_bit_index = (uint64_t)best;
    sr->tag_bit_offset = (uint64_t)offset;
    sr->tag_bit_mask = _tag_mask(nt, (uint64_t)offset);
    bucket_remaining[best] -= nt;
  }

  cl->bits_bucket_size = bucket_size;
  free(entries);
  free(bucket_remaining);
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

// Use pre-computed ScopedRule.nullable for callee lookups.
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

static void _compute_first_set(ScopeClosure* cl, ScopedUnit* unit, Bitset* set) {
  switch (unit->kind) {
  case SCOPED_UNIT_TERM:
    bitset_add_bit(set, (uint32_t)unit->as.term_id);
    break;
  case SCOPED_UNIT_CALL: {
    int32_t cid = symtab_find(&cl->scoped_rule_names, unit->as.callee);
    if (cid >= 0) {
      int32_t idx = cid - cl->scoped_rule_names.start_num;
      if (cl->scoped_rules[idx].first_set) {
        bitset_or_into(set, cl->scoped_rules[idx].first_set);
      }
    }
    break;
  }
  case SCOPED_UNIT_SEQ:
    for (size_t i = 0; i < darray_size(unit->as.children); i++) {
      _compute_first_set(cl, &unit->as.children[i], set);
      if (!_is_unit_nullable(cl, &unit->as.children[i])) {
        break;
      }
    }
    break;
  case SCOPED_UNIT_BRANCHES:
    for (size_t i = 0; i < darray_size(unit->as.children); i++) {
      _compute_first_set(cl, &unit->as.children[i], set);
    }
    break;
  case SCOPED_UNIT_MAYBE:
    _compute_first_set(cl, unit->as.base, set);
    break;
  case SCOPED_UNIT_STAR:
  case SCOPED_UNIT_PLUS:
    _compute_first_set(cl, unit->as.interlace.lhs, set);
    break;
  }
}

static void _compute_last_set(ScopeClosure* cl, ScopedUnit* unit, Bitset* set) {
  switch (unit->kind) {
  case SCOPED_UNIT_TERM:
    bitset_add_bit(set, (uint32_t)unit->as.term_id);
    break;
  case SCOPED_UNIT_CALL: {
    int32_t cid = symtab_find(&cl->scoped_rule_names, unit->as.callee);
    if (cid >= 0) {
      int32_t idx = cid - cl->scoped_rule_names.start_num;
      if (cl->scoped_rules[idx].last_set) {
        bitset_or_into(set, cl->scoped_rules[idx].last_set);
      }
    }
    break;
  }
  case SCOPED_UNIT_SEQ: {
    int32_t n = (int32_t)darray_size(unit->as.children);
    for (int32_t i = n - 1; i >= 0; i--) {
      _compute_last_set(cl, &unit->as.children[i], set);
      if (!_is_unit_nullable(cl, &unit->as.children[i])) {
        break;
      }
    }
    break;
  }
  case SCOPED_UNIT_BRANCHES:
    for (size_t i = 0; i < darray_size(unit->as.children); i++) {
      _compute_last_set(cl, &unit->as.children[i], set);
    }
    break;
  case SCOPED_UNIT_MAYBE:
    _compute_last_set(cl, unit->as.base, set);
    break;
  case SCOPED_UNIT_STAR:
  case SCOPED_UNIT_PLUS:
    _compute_last_set(cl, unit->as.interlace.lhs, set);
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

  // Phase 1: fixpoint for nullable / min_size / max_size
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

  // Phase 2: fixpoint for first_set / last_set
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
      _compute_first_set(cl, &cl->scoped_rules[i].body, cl->scoped_rules[i].first_set);
      _compute_last_set(cl, &cl->scoped_rules[i].body, cl->scoped_rules[i].last_set);
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

  // Phase 3: stamp ScopedUnit.nullable on each unit tree
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
    cl->scoped_rules[i].segment_index = (uint64_t)sg_id;
    cl->scoped_rules[i].rule_bit_mask = (uint64_t)seg_mask;
    cl->scoped_rules[i].slot_index = (uint64_t)sg_id;
  }
  // compute per-segment union mask: OR of all rule_bit_mask values sharing the same segment_index
  for (int32_t i = 0; i < n; i++) {
    uint64_t seg_union = 0;
    uint64_t seg_idx = cl->scoped_rules[i].segment_index;
    for (int32_t j = 0; j < n; j++) {
      if (cl->scoped_rules[j].segment_index == seg_idx) {
        seg_union |= cl->scoped_rules[j].rule_bit_mask;
      }
    }
    cl->scoped_rules[i].segment_mask = seg_union;
  }
  coloring_result_del(cr);
}

// Spec: fill gaps in existing segment bit buckets with tag bits.
// Each segment occupies one uint64_t bucket in Col.bits[]. The segment mask uses
// some bits; tag bits for rules in that segment can use the remaining bits.
// Rules sharing a segment share tag_bit_index/mask/offset.
static void _alloc_shared_tag_bits(ScopeClosure* cl) {
  int32_t rule_size = (int32_t)darray_size(cl->scoped_rules);
  int32_t segment_size = (int32_t)cl->slots_size;

  // count used bits per segment (from segment_mask), and max tags per segment
  int32_t* seg_used_bits = calloc((size_t)segment_size, sizeof(int32_t));
  int32_t* seg_max_tags = calloc((size_t)segment_size, sizeof(int32_t));
  for (int32_t i = 0; i < rule_size; i++) {
    int32_t seg = (int32_t)cl->scoped_rules[i].segment_index;
    if (seg < segment_size) {
      seg_used_bits[seg] = (int32_t)__builtin_popcountll(cl->scoped_rules[i].segment_mask);
      int32_t nt = symtab_count(&cl->scoped_rules[i].tags);
      if (nt > seg_max_tags[seg]) {
        seg_max_tags[seg] = nt;
      }
    }
  }

  int64_t next_bucket = segment_size; // first bucket after segment buckets
  for (int32_t seg = 0; seg < segment_size; seg++) {
    int32_t max_tags = seg_max_tags[seg];
    if (max_tags == 0) {
      continue;
    }
    int32_t remaining = 64 - seg_used_bits[seg];
    uint64_t tag_bit_index;
    uint64_t tag_bit_offset;
    if (max_tags <= remaining) {
      // fits in the same bucket as the segment bitmask
      tag_bit_index = (uint64_t)seg;
      tag_bit_offset = (uint64_t)seg_used_bits[seg];
    } else {
      // need a new bucket
      tag_bit_index = (uint64_t)next_bucket;
      tag_bit_offset = 0;
      next_bucket++;
    }
    uint64_t tag_bit_mask = _tag_mask(max_tags, tag_bit_offset);
    // assign to all rules in this segment
    for (int32_t i = 0; i < rule_size; i++) {
      if ((int32_t)cl->scoped_rules[i].segment_index == seg) {
        cl->scoped_rules[i].tag_bit_index = tag_bit_index;
        cl->scoped_rules[i].tag_bit_offset = tag_bit_offset;
        cl->scoped_rules[i].tag_bit_mask = tag_bit_mask;
      }
    }
  }
  // zero out tag bits for rules with no tags
  for (int32_t i = 0; i < rule_size; i++) {
    if (symtab_count(&cl->scoped_rules[i].tags) == 0) {
      cl->scoped_rules[i].tag_bit_index = 0;
      cl->scoped_rules[i].tag_bit_mask = 0;
      cl->scoped_rules[i].tag_bit_offset = 0;
    }
  }
  cl->bits_bucket_size = next_bucket;

  free(seg_used_bits);
  free(seg_max_tags);
}

// ============================================================
// Field name deduplication: foo, foo$1, foo$2, ...
// ============================================================

// Track occurrence counts for field names. Simple linear scan since field counts are small.
typedef struct {
  char** names; // darray
} FieldDedup;

static void _field_dedup_init(FieldDedup* fd) { fd->names = darray_new(sizeof(char*), 0); }

static void _field_dedup_free(FieldDedup* fd) {
  for (size_t i = 0; i < darray_size(fd->names); i++) {
    free(fd->names[i]);
  }
  darray_del(fd->names);
}

// Record one occurrence, return the dedup'd name (malloc'd).
// First occurrence of "x" -> "x", second -> "x$1", third -> "x$2".
static char* _field_dedup_next(FieldDedup* fd, const char* base_name) {
  int32_t occ = 0;
  for (size_t i = 0; i < darray_size(fd->names); i++) {
    if (strcmp(fd->names[i], base_name) == 0) {
      occ++;
    }
  }
  char* copy = strdup(base_name);
  darray_push(fd->names, copy);
  if (occ == 0) {
    return strdup(base_name);
  }
  return _fmt("%s$%d", base_name, occ);
}

// ============================================================
// Field name sanitization: @lit.while -> _lit_while
// ============================================================

static char* _sanitize_field_name(const char* name) {
  size_t len = strlen(name);
  char* out = malloc(len + 2);
  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    char c = name[i];
    out[j++] = (c == '@' || c == '.') ? '_' : c;
  }
  out[j] = '\0';
  return out;
}

// ============================================================
// Header generation
// ============================================================

// Emit types needed by PEG inline loaders. Minimal Token/TokenChunk/TokenTree definitions
// are guarded so the VPA runtime amalgamation (which includes the full token_tree.h) won't conflict.
static void _gen_header_types(HeaderWriter* hw) {
  hw_raw(hw, "#include <stdint.h>\n#include <stdbool.h>\n#include <string.h>\n\n");
  hw_raw(hw, "#ifndef _NEST_TOKEN_TYPES\n#define _NEST_TOKEN_TYPES\n");
  hw_raw(hw, "typedef struct { int32_t term_id; int32_t cp_start; int32_t cp_size; int32_t chunk_id; } Token;\n");
  hw_raw(hw, "typedef Token* Tokens;\n");
  hw_raw(hw, "typedef struct TokenChunk {\n"
             "  int32_t scope_id; int32_t parent_id;\n"
             "  void* value; void* aux_value; Tokens tokens;\n"
             "} TokenChunk;\n");
  hw_raw(hw, "typedef TokenChunk* TokenChunks;\n");
  hw_raw(hw, "typedef struct TokenTree {\n"
             "  const char* src; uint64_t* newline_map;\n"
             "  TokenChunk* root; TokenChunk* current; TokenChunks table;\n"
             "} TokenTree;\n");
  hw_raw(hw, "#endif\n\n");
  hw_raw(hw, "#ifndef _NEST_PEGREF\n#define _NEST_PEGREF\n");
  hw_raw(hw, "typedef struct { TokenChunk* tc; int64_t col; int64_t row; } PegRef;\n");
  hw_raw(hw, "#endif\n\n");
  hw_raw(hw, "typedef struct {\n  int64_t rhs_row;\n  int64_t col_size_in_i32;\n  PegRef elem;\n} PegLink;\n");
  hw_blank(hw);
}

// Look up a child's ScopedRule in the closure by scoped name. Returns NULL if not found.
static ScopedRule* _find_scoped_rule(ScopeClosure* cl, int32_t id) {
  if (id < 0) {
    return NULL;
  }
  return &cl->scoped_rules[id - cl->scoped_rule_names.start_num];
}

// Compute the row index (i32 offset from column start) for a scoped rule's slot.
// Col layout: bits[bits_bucket_size] as i64 (= bits_bucket_size*2 i32s), then slots[] as i32.
static int32_t _slot_row(ScopeClosure* cl, ScopedRule* sr) {
  return (int32_t)(cl->bits_bucket_size * 2 + (int64_t)sr->slot_index);
}

// Emit a PegUnit child's fields. Used by both SEQ and BRANCHES cases.
// Emits PegRef or PegLink fields with FieldDedup. For non-link children, the col
// is read from the hw-side `_cur` variable that tracks the running token position.
// For PEG_TERM: always 1 token wide, no slot lookup needed.
// For PEG_CALL without multiplier: read slot to advance cur.
// For PEG_CALL with multiplier (PegLink): set elem.row + rhs_row from wrapper rule.
static void _gen_child_fields_struct(HeaderWriter* hw, PegGenInput* input, PegUnit* children, int32_t child_size,
                                     FieldDedup* fd) {
  for (int32_t i = 0; i < child_size; i++) {
    PegUnit* u = &children[i];
    if (u->kind == PEG_TERM) {
      bool is_link = (u->multiplier == '*' || u->multiplier == '+');
      const char* type = is_link ? "PegLink" : "PegRef";
      char* san = _sanitize_field_name(_term_name(input, u->id));
      char* dedup = _field_dedup_next(fd, san);
      hw_fmt(hw, "  %s %s;\n", type, dedup);
      free(dedup);
      free(san);
    } else if (u->kind == PEG_CALL) {
      bool is_link = (u->multiplier == '*' || u->multiplier == '+');
      const char* type = is_link ? "PegLink" : "PegRef";
      const char* base = symtab_get(&input->rule_names, u->id);
      char* dedup = _field_dedup_next(fd, base);
      hw_fmt(hw, "  %s %s;\n", type, dedup);
      free(dedup);
    } else if (u->kind == PEG_BRANCHES) {
      for (size_t j = 0; j < darray_size(u->children); j++) {
        PegUnit* branch = &u->children[j];
        if (branch->kind == PEG_SEQ) {
          _gen_child_fields_struct(hw, input, branch->children, (int32_t)darray_size(branch->children), fd);
        } else if (branch->kind == PEG_TERM || branch->kind == PEG_CALL) {
          _gen_child_fields_struct(hw, input, branch, 1, fd);
        }
      }
    }
  }
}

// Emit Node_xxx struct for a rule.
static void _gen_node_struct(HeaderWriter* hw, PegGenInput* input, RuleLookup* lu, ScopedRule* sr,
                             const char* rule_name) {
  PegRule* orig = _lookup_rule(lu, sr->original_global_id);
  if (!orig) {
    return;
  }
  int32_t tag_size = symtab_count(&sr->tags);
  hw_fmt(hw, "typedef struct {\n");
  if (tag_size > 0) {
    hw_raw(hw, "  struct {\n");
    for (int32_t t = 0; t < tag_size; t++) {
      char* san = _sanitize_field_name(symtab_get(&sr->tags, t + sr->tags.start_num));
      hw_fmt(hw, "    bool %s : 1;\n", san);
      free(san);
    }
    if (tag_size < 64) {
      hw_fmt(hw, "    uint64_t _padding : %d;\n", 64 - tag_size);
    }
    hw_raw(hw, "  } is;\n");
  }
  if (orig->body.kind == PEG_SEQ) {
    FieldDedup fd;
    _field_dedup_init(&fd);
    _gen_child_fields_struct(hw, input, orig->body.children, (int32_t)darray_size(orig->body.children), &fd);
    _field_dedup_free(&fd);
  } else if (orig->body.kind == PEG_BRANCHES) {
    FieldDedup fd;
    _field_dedup_init(&fd);
    for (size_t i = 0; i < darray_size(orig->body.children); i++) {
      PegUnit* branch = &orig->body.children[i];
      if (branch->kind == PEG_SEQ) {
        _gen_child_fields_struct(hw, input, branch->children, (int32_t)darray_size(branch->children), &fd);
      } else if (branch->kind == PEG_TERM || branch->kind == PEG_CALL) {
        _gen_child_fields_struct(hw, input, branch, 1, &fd);
      }
    }
    _field_dedup_free(&fd);
  } else if (orig->body.kind == PEG_TERM) {
    bool is_link = (orig->body.multiplier == '*' || orig->body.multiplier == '+');
    const char* type = is_link ? "PegLink" : "PegRef";
    char* san = _sanitize_field_name(_term_name(input, orig->body.id));
    hw_fmt(hw, "  %s %s;\n", type, san);
    free(san);
  } else if (orig->body.kind == PEG_CALL) {
    bool is_link = (orig->body.multiplier == '*' || orig->body.multiplier == '+');
    const char* type = is_link ? "PegLink" : "PegRef";
    hw_fmt(hw, "  %s %s;\n", type, symtab_get(&input->rule_names, orig->body.id));
  }
  hw_fmt(hw, "} Node_%s;\n", rule_name);
  hw_blank(hw);
}

// Info for one scope's instance of a rule, used in loader generation.
typedef struct {
  int32_t scope_id;
  ScopedRule* sr;
  ScopeClosure* cl;
} RuleScopeEntry;

// Pre-computed mapping: original_global_id -> array of RuleScopeEntry
typedef struct {
  RuleScopeEntry** entries; // entries[gid - start_num] = array (or NULL)
  int32_t* counts;          // counts[gid - start_num]
  int32_t start_num;
  int32_t size;
} RuleScopeMap;

static RuleScopeMap _build_scope_map(PegGenInput* input, ScopeClosure* closures, int32_t closure_size) {
  RuleScopeMap map = {
      .start_num = input->rule_names.start_num,
      .size = symtab_count(&input->rule_names),
  };
  map.entries = calloc((size_t)map.size, sizeof(RuleScopeEntry*));
  map.counts = calloc((size_t)map.size, sizeof(int32_t));
  for (int32_t c = 0; c < closure_size; c++) {
    ScopeClosure* cl = &closures[c];
    for (size_t i = 0; i < darray_size(cl->scoped_rules); i++) {
      ScopedRule* sr = &cl->scoped_rules[i];
      if (sr->original_global_id < 0) {
        continue;
      }
      int32_t idx = sr->original_global_id - map.start_num;
      if (idx < 0 || idx >= map.size) {
        continue;
      }
      int32_t n = map.counts[idx];
      map.entries[idx] = realloc(map.entries[idx], (size_t)(n + 1) * sizeof(RuleScopeEntry));
      map.entries[idx][n] = (RuleScopeEntry){.scope_id = cl->scope_id, .sr = sr, .cl = cl};
      map.counts[idx] = n + 1;
    }
  }
  return map;
}

static void _free_scope_map(RuleScopeMap* map) {
  for (int32_t i = 0; i < map->size; i++) {
    free(map->entries[i]);
  }
  free(map->entries);
  free(map->counts);
}

// Emit loader field assignments for a sequence of PegUnit children.
// Writes into the hw-generated loader body, using a `_cur` variable that tracks
// the running token position. After each child, advances _cur by reading the slot.
static void _gen_child_fields_loader(HeaderWriter* hw, PegGenInput* input, PegUnit* children, int32_t child_size,
                                     FieldDedup* fd, ScopeClosure* cl, const char* parent_rule_name,
                                     int32_t* multiplier_num_ptr) {
  int64_t col_size_in_i32 = cl->bits_bucket_size * 2 + cl->slots_size;

  for (int32_t i = 0; i < child_size; i++) {
    PegUnit* u = &children[i];
    bool is_link = (u->multiplier == '*' || u->multiplier == '+');

    if (u->kind == PEG_TERM) {
      char* san = _sanitize_field_name(_term_name(input, u->id));
      char* dedup = _field_dedup_next(fd, san);
      bool is_scope = _is_scope_term(input, u->id);
      if (is_link) {
        (*multiplier_num_ptr)++;
        ScopedRule* wrapper = _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s$%d", cl->scope_name,
                                                                  parent_rule_name, *multiplier_num_ptr));
        hw_fmt(hw, "  n.%s.elem.tc = ref.tc;\n", dedup);
        hw_fmt(hw, "  n.%s.elem.col = _cur;\n", dedup);
        hw_fmt(hw, "  n.%s.elem.row = %d;\n", dedup, wrapper ? _slot_row(cl, wrapper) : 0);
        hw_fmt(hw, "  n.%s.col_size_in_i32 = %lld;\n", dedup, (long long)col_size_in_i32);
        hw_fmt(hw, "  n.%s.rhs_row = -1;\n", dedup);
      } else if (is_scope) {
        hw_fmt(hw, "  n.%s.tc = &((TokenTree*)ref.tc->aux_value)->table[ref.tc->tokens[_cur].chunk_id];\n", dedup);
        hw_fmt(hw, "  n.%s.col = 0;\n", dedup);
        hw_fmt(hw, "  n.%s.row = 0;\n", dedup);
      } else {
        hw_fmt(hw, "  n.%s.tc = ref.tc;\n", dedup);
        hw_fmt(hw, "  n.%s.col = _cur;\n", dedup);
        hw_fmt(hw, "  n.%s.row = 0;\n", dedup);
      }
      hw_raw(hw, "  _cur++;\n");
      free(dedup);
      free(san);
    } else if (u->kind == PEG_CALL) {
      const char* callee_name = symtab_get(&input->rule_names, u->id);
      char* dedup = _field_dedup_next(fd, callee_name);

      if (is_link) {
        (*multiplier_num_ptr)++;
        ScopedRule* wrapper = _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s$%d", cl->scope_name,
                                                                  parent_rule_name, *multiplier_num_ptr));
        int32_t wrapper_row = wrapper ? _slot_row(cl, wrapper) : 0;

        int32_t rhs_row = -1;
        if (u->interlace_rhs_kind == PEG_CALL) {
          const char* rhs_name = symtab_get(&input->rule_names, u->interlace_rhs_id);
          ScopedRule* rhs_sr =
              _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s", cl->scope_name, rhs_name));
          if (rhs_sr) {
            rhs_row = _slot_row(cl, rhs_sr);
          }
        }

        hw_fmt(hw, "  n.%s.elem.tc = ref.tc;\n", dedup);
        hw_fmt(hw, "  n.%s.elem.col = _cur;\n", dedup);
        hw_fmt(hw, "  n.%s.elem.row = %d;\n", dedup, wrapper_row);
        hw_fmt(hw, "  n.%s.col_size_in_i32 = %lld;\n", dedup, (long long)col_size_in_i32);
        hw_fmt(hw, "  n.%s.rhs_row = %d;\n", dedup, rhs_row);
      } else {
        ScopedRule* callee_sr =
            _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s", cl->scope_name, callee_name));
        int32_t callee_row = callee_sr ? _slot_row(cl, callee_sr) : 0;

        hw_fmt(hw, "  n.%s.tc = ref.tc;\n", dedup);
        hw_fmt(hw, "  n.%s.col = _cur;\n", dedup);
        hw_fmt(hw, "  n.%s.row = %d;\n", dedup, callee_row);
      }

      // advance _cur by reading the child's matched size from the memo table
      ScopedRule* child_sr =
          _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s", cl->scope_name, callee_name));
      if (child_sr) {
        hw_fmt(hw, "  _cur += ((int32_t*)ref.tc->value)[_cur * %lld + %d];\n", (long long)col_size_in_i32,
               _slot_row(cl, child_sr));
      }

      free(dedup);
    } else if (u->kind == PEG_BRANCHES) {
      // branch alternatives all start at _cur; set each alternative's children to _cur
      for (size_t j = 0; j < darray_size(u->children); j++) {
        PegUnit* branch = &u->children[j];
        if (branch->kind == PEG_TERM) {
          char* san = _sanitize_field_name(_term_name(input, branch->id));
          char* dedup = _field_dedup_next(fd, san);
          if (_is_scope_term(input, branch->id)) {
            hw_fmt(hw, "  n.%s.tc = &((TokenTree*)ref.tc->aux_value)->table[ref.tc->tokens[_cur].chunk_id];\n", dedup);
            hw_fmt(hw, "  n.%s.col = 0;\n", dedup);
          } else {
            hw_fmt(hw, "  n.%s.tc = ref.tc;\n", dedup);
            hw_fmt(hw, "  n.%s.col = _cur;\n", dedup);
          }
          hw_fmt(hw, "  n.%s.row = 0;\n", dedup);
          free(dedup);
          free(san);
        } else if (branch->kind == PEG_CALL) {
          const char* callee_name = symtab_get(&input->rule_names, branch->id);
          char* dedup = _field_dedup_next(fd, callee_name);
          ScopedRule* callee_sr =
              _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s", cl->scope_name, callee_name));
          int32_t callee_row = callee_sr ? _slot_row(cl, callee_sr) : 0;
          hw_fmt(hw, "  n.%s.tc = ref.tc;\n", dedup);
          hw_fmt(hw, "  n.%s.col = _cur;\n", dedup);
          hw_fmt(hw, "  n.%s.row = %d;\n", dedup, callee_row);
          free(dedup);
        } else if (branch->kind == PEG_SEQ) {
          for (size_t k = 0; k < darray_size(branch->children); k++) {
            PegUnit* bc = &branch->children[k];
            if (bc->kind == PEG_TERM) {
              char* san = _sanitize_field_name(_term_name(input, bc->id));
              char* dedup = _field_dedup_next(fd, san);
              if (_is_scope_term(input, bc->id)) {
                hw_fmt(hw, "  n.%s.tc = &((TokenTree*)ref.tc->aux_value)->table[ref.tc->tokens[_cur].chunk_id];\n",
                       dedup);
                hw_fmt(hw, "  n.%s.col = 0;\n", dedup);
              } else {
                hw_fmt(hw, "  n.%s.tc = ref.tc;\n", dedup);
                hw_fmt(hw, "  n.%s.col = _cur;\n", dedup);
              }
              hw_fmt(hw, "  n.%s.row = 0;\n", dedup);
              free(dedup);
              free(san);
            } else if (bc->kind == PEG_CALL) {
              const char* cn = symtab_get(&input->rule_names, bc->id);
              char* dedup = _field_dedup_next(fd, cn);
              ScopedRule* csr =
                  _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s", cl->scope_name, cn));
              int32_t crow = csr ? _slot_row(cl, csr) : 0;
              hw_fmt(hw, "  n.%s.tc = ref.tc;\n", dedup);
              hw_fmt(hw, "  n.%s.col = _cur;\n", dedup);
              hw_fmt(hw, "  n.%s.row = %d;\n", dedup, crow);
              free(dedup);
            }
          }
        }
      }
      // advance _cur past the branch by reading the wrapper rule's slot
      (*multiplier_num_ptr)++;
      ScopedRule* wrapper = _find_scoped_rule(
          cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s$%d", cl->scope_name, parent_rule_name, *multiplier_num_ptr));
      if (wrapper) {
        hw_fmt(hw, "  _cur += ((int32_t*)ref.tc->value)[_cur * %lld + %d];\n", (long long)col_size_in_i32,
               _slot_row(cl, wrapper));
      }
    }
  }
}

// Emit loader function. Always uses switch(ref.tc->scope_id) for both tag
// decoding and child field assignments, since different scopes have independent numbering.
static void _gen_loader(HeaderWriter* hw, PegGenInput* input, RuleLookup* lu, const char* rule_name,
                        RuleScopeEntry* scope_entries, int32_t used_in_closures_count, const char* prefix) {
  if (used_in_closures_count == 0) {
    return;
  }
  PegRule* orig = _lookup_rule(lu, scope_entries[0].sr->original_global_id);
  if (!orig) {
    return;
  }
  int32_t tag_size = symtab_count(&scope_entries[0].sr->tags);
  bool has_tags = (tag_size > 0);

  hw_fmt(hw, "static inline Node_%s %s_load_%s(PegRef ref) {\n", rule_name, prefix, rule_name);
  hw_fmt(hw, "  Node_%s n;\n", rule_name);
  hw_raw(hw, "  memset(&n, 0, sizeof(n));\n");
  hw_raw(hw, "  int64_t* table = (int64_t*)ref.tc->value;\n");
  hw_raw(hw, "  int64_t _cur = ref.col;\n");

  hw_raw(hw, "  switch (ref.tc->scope_id) {\n");
  for (int32_t e = 0; e < used_in_closures_count; e++) {
    ScopedRule* sr = scope_entries[e].sr;
    ScopeClosure* cl = scope_entries[e].cl;
    int64_t col_size_in_i32 = cl->bits_bucket_size * 2 + cl->slots_size;

    hw_fmt(hw, "  case %d: {\n", scope_entries[e].scope_id);

    // tag decoding: col = table + ref.col * col_size_in_i64
    if (has_tags) {
      hw_fmt(hw, "    int64_t* col = (int64_t*)((int32_t*)table + %lld * ref.col);\n", (long long)col_size_in_i32);
      hw_fmt(hw, "    ((uint64_t*)&n.is)[0] = (col[%llu] >> %lluULL) & 0x%llxULL;\n",
             (unsigned long long)sr->tag_bit_index, (unsigned long long)sr->tag_bit_offset,
             (unsigned long long)_tag_mask(tag_size, 0));
    }

    // child field assignments using this scope's closure
    if (orig->body.kind == PEG_SEQ) {
      FieldDedup fd;
      _field_dedup_init(&fd);
      int32_t multiplier_num = 0;
      _gen_child_fields_loader(hw, input, orig->body.children, (int32_t)darray_size(orig->body.children), &fd, cl,
                               rule_name, &multiplier_num);
      _field_dedup_free(&fd);
    } else if (orig->body.kind == PEG_BRANCHES) {
      FieldDedup fd;
      _field_dedup_init(&fd);
      for (size_t i = 0; i < darray_size(orig->body.children); i++) {
        PegUnit* branch = &orig->body.children[i];
        int32_t multiplier_num = 0;
        if (branch->kind == PEG_SEQ) {
          _gen_child_fields_loader(hw, input, branch->children, (int32_t)darray_size(branch->children), &fd, cl,
                                   rule_name, &multiplier_num);
        } else if (branch->kind == PEG_TERM || branch->kind == PEG_CALL) {
          _gen_child_fields_loader(hw, input, branch, 1, &fd, cl, rule_name, &multiplier_num);
        }
      }
      _field_dedup_free(&fd);
    } else if (orig->body.kind == PEG_TERM) {
      bool is_link = (orig->body.multiplier == '*' || orig->body.multiplier == '+');
      char* san = _sanitize_field_name(_term_name(input, orig->body.id));
      if (is_link) {
        int64_t col_size_in_i32 = cl->bits_bucket_size * 2 + cl->slots_size;
        ScopedRule* wrapper = _find_scoped_rule(
            cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s$%d", cl->scope_name, rule_name, 1));
        int32_t wrapper_row = wrapper ? _slot_row(cl, wrapper) : 0;
        if (_is_scope_term(input, orig->body.id)) {
          hw_fmt(hw,
                 "    n.%s.elem.tc = &((TokenTree*)ref.tc->aux_value)->table[ref.tc->tokens[ref.col].chunk_id];\n",
                 san);
          hw_fmt(hw, "    n.%s.elem.col = 0;\n", san);
        } else {
          hw_fmt(hw, "    n.%s.elem.tc = ref.tc;\n    n.%s.elem.col = ref.col;\n", san, san);
        }
        hw_fmt(hw, "    n.%s.elem.row = %d;\n", san, wrapper_row);
        hw_fmt(hw, "    n.%s.col_size_in_i32 = %lld;\n    n.%s.rhs_row = -1;\n", san, (long long)col_size_in_i32,
               san);
      } else if (_is_scope_term(input, orig->body.id)) {
        hw_fmt(hw, "    n.%s.tc = &((TokenTree*)ref.tc->aux_value)->table[ref.tc->tokens[ref.col].chunk_id];\n", san);
        hw_fmt(hw, "    n.%s.col = 0;\n    n.%s.row = 0;\n", san, san);
      } else {
        hw_fmt(hw, "    n.%s.tc = ref.tc;\n    n.%s.col = ref.col;\n    n.%s.row = 0;\n", san, san, san);
      }
      free(san);
    } else if (orig->body.kind == PEG_CALL) {
      bool is_link = (orig->body.multiplier == '*' || orig->body.multiplier == '+');
      const char* fn = symtab_get(&input->rule_names, orig->body.id);
      if (is_link) {
        int64_t col_size_in_i32 = cl->bits_bucket_size * 2 + cl->slots_size;
        ScopedRule* wrapper = _find_scoped_rule(
            cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s$%d", cl->scope_name, rule_name, 1));
        int32_t wrapper_row = wrapper ? _slot_row(cl, wrapper) : 0;
        hw_fmt(hw, "    n.%s.elem.tc = ref.tc;\n    n.%s.elem.col = ref.col;\n", fn, fn);
        hw_fmt(hw, "    n.%s.elem.row = %d;\n", fn, wrapper_row);
        hw_fmt(hw, "    n.%s.col_size_in_i32 = %lld;\n    n.%s.rhs_row = -1;\n", fn, (long long)col_size_in_i32, fn);
      } else {
        ScopedRule* callee_sr =
            _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s", cl->scope_name, fn));
        int32_t callee_row = callee_sr ? _slot_row(cl, callee_sr) : 0;
        hw_fmt(hw, "    n.%s.tc = ref.tc;\n    n.%s.col = ref.col;\n    n.%s.row = %d;\n", fn, fn, fn, callee_row);
      }
    }

    hw_raw(hw, "    break;\n  }\n");
  }
  hw_raw(hw, "  }\n");

  hw_raw(hw, "  return n;\n}\n");
  hw_blank(hw);
}

static void _gen_has_next(HeaderWriter* hw, const char* prefix) {
  hw_fmt(hw, "static inline bool %s_has_next(PegLink l) {\n", prefix);
  hw_raw(hw, "  int32_t* col = (int32_t*)l.elem.tc->value + l.col_size_in_i32 * l.elem.col;\n");
  hw_raw(hw, "  int32_t lhs_slot = col[l.elem.row];\n");
  hw_raw(hw, "  if (lhs_slot < 0) return false;\n");
  hw_raw(hw, "  if (l.rhs_row >= 0) {\n");
  hw_raw(hw, "    int32_t* rhs_col = col + l.col_size_in_i32 * lhs_slot;\n");
  hw_raw(hw, "    if (rhs_col[l.rhs_row] < 0) return false;\n");
  hw_raw(hw, "  }\n");
  hw_raw(hw, "  return true;\n}\n");
  hw_blank(hw);
}

static void _gen_get_next(HeaderWriter* hw, const char* prefix) {
  hw_fmt(hw, "static inline PegLink %s_get_next(PegLink l) {\n", prefix);
  hw_raw(hw, "  int32_t* col = (int32_t*)l.elem.tc->value + l.col_size_in_i32 * l.elem.col;\n");
  hw_raw(hw, "  int32_t lhs_slot = col[l.elem.row];\n");
  hw_raw(hw, "  PegLink next = l;\n");
  hw_raw(hw, "  if (l.rhs_row >= 0) {\n");
  hw_raw(hw, "    int32_t* rhs_col = col + l.col_size_in_i32 * lhs_slot;\n");
  hw_raw(hw, "    next.elem.col = l.elem.col + lhs_slot + rhs_col[l.rhs_row];\n");
  hw_raw(hw, "  } else {\n");
  hw_raw(hw, "    next.elem.col = l.elem.col + lhs_slot;\n");
  hw_raw(hw, "  }\n");
  hw_raw(hw, "  return next;\n}\n");
  hw_blank(hw);
}

static void _gen_peg_size(HeaderWriter* hw, ScopeClosure* closures, int32_t closure_size, const char* prefix) {
  hw_fmt(hw, "static inline int32_t %s_peg_size(PegRef ref) {\n", prefix);
  hw_raw(hw, "  if (!ref.tc || !ref.tc->value) return -1;\n");
  hw_raw(hw, "  int64_t col_size;\n");
  hw_raw(hw, "  switch (ref.tc->scope_id) {\n");
  for (int32_t c = 0; c < closure_size; c++) {
    int64_t col_size = closures[c].bits_bucket_size * 2 + closures[c].slots_size;
    hw_fmt(hw, "  case %d: col_size = %lld; break;\n", closures[c].scope_id, (long long)col_size);
  }
  hw_raw(hw, "  default: return -1;\n");
  hw_raw(hw, "  }\n");
  hw_raw(hw, "  return ((int32_t*)ref.tc->value)[ref.col * col_size + ref.row];\n");
  hw_raw(hw, "}\n");
  hw_blank(hw);
}

static void _gen_header(PegGenInput* input, RuleLookup* lu, HeaderWriter* hw, ScopeClosure* closures,
                        int32_t closure_size, RuleScopeMap* scope_map, const char* prefix) {
  _gen_header_types(hw);

  // Collect unique original rule names across all closures.
  Symtab emitted_rules;
  symtab_init(&emitted_rules, 0);

  for (int32_t c = 0; c < closure_size; c++) {
    ScopeClosure* cl = &closures[c];
    for (size_t i = 0; i < darray_size(cl->scoped_rules); i++) {
      ScopedRule* sr = &cl->scoped_rules[i];
      if (sr->original_global_id < 0) {
        continue;
      }
      const char* rule_name = symtab_get(&input->rule_names, sr->original_global_id);
      int32_t emitted_id = symtab_find(&emitted_rules, rule_name);
      if (emitted_id >= 0) {
        continue;
      }
      symtab_intern(&emitted_rules, rule_name);

      _gen_node_struct(hw, input, lu, sr, rule_name);

      int32_t map_idx = sr->original_global_id - scope_map->start_num;
      RuleScopeEntry* scope_entries = scope_map->entries[map_idx];
      int32_t used_in_closures_count = scope_map->counts[map_idx];
      _gen_loader(hw, input, lu, rule_name, scope_entries, used_in_closures_count, prefix);
    }
  }
  symtab_free(&emitted_rules);

  _gen_has_next(hw, prefix);
  _gen_get_next(hw, prefix);
  _gen_peg_size(hw, closures, closure_size, prefix);
}

// ============================================================
// IR code generation
// ============================================================

static void _gen_scope_ir(IrWriter* w, ScopeClosure* cl, int memoize_mode) {
  int32_t rule_size = (int32_t)darray_size(cl->scoped_rules);
  if (rule_size == 0) {
    return;
  }
  int64_t sizeof_col = cl->bits_bucket_size * 8 + cl->slots_size * 4;

  irwriter_declare(w, "ptr", "tt_current", "ptr");
  irwriter_declare(w, "ptr", "tt_alloc_memoize_table", "ptr, i64");
  irwriter_declare(w, "i32", "tt_current_size", "ptr");

  char* fn_name = _fmt("parse_%s", cl->scope_name);
  irwriter_define_startf(w, fn_name, "{i64, i64} @%s(ptr %%tt, ptr %%stack_ptr_in)", fn_name);
  irwriter_bb(w);
  irwriter_dbg(w, 1, 0);

  IrVal tc = irwriter_call_retf(w, "ptr", "tt_current", "ptr %%tt");
  // tt_alloc_memoize_table already initializes to -1 via memset
  IrVal peg_table =
      irwriter_call_retf(w, "ptr", "tt_alloc_memoize_table", "ptr %%r%d, i64 %lld", (int)tc, (long long)sizeof_col);

  IrVal col_ptr = irwriter_alloca(w, "i64");
  irwriter_store(w, "i64", irwriter_imm_int(w, 0), col_ptr);
  IrVal stack_ptr = irwriter_alloca(w, "ptr");
  irwriter_store(w, "ptr", irwriter_imm(w, "%stack_ptr_in"), stack_ptr);
  IrVal parse_result = irwriter_alloca(w, "i64");
  irwriter_store(w, "i64", irwriter_imm_int(w, -1), parse_result);
  IrVal parsed_tokens = irwriter_alloca(w, "i64");
  irwriter_store(w, "i64", irwriter_imm_int(w, 0), parsed_tokens);
  IrVal tag_bits = irwriter_alloca(w, "i64");
  irwriter_store(w, "i64", irwriter_imm_int(w, 0), tag_bits);

  IrVal token_size = irwriter_call_retf(w, "i32", "tt_current_size", "ptr %%tt");

  // tc->tokens at offset 24 (scope_id:4, parent_id:4, value:8, aux_value:8)
  irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%r%d, i64 24\n", irwriter_next_reg(w), (int)tc);
  IrVal tokens = irwriter_load(w, "ptr", (IrVal)(irwriter_next_reg(w) - 1));

  PegIrCtx ctx = {
      .ir_writer = w,
      .fn_name = fn_name,
      .tc = tc,
      .tokens = tokens,
      .col = col_ptr,
      .token_size = token_size,
      .stack_ptr = stack_ptr,
      .parse_result = parse_result,
      .tag_bits = tag_bits,
      .parsed_tokens = parsed_tokens,
      .ret_labels = darray_new(sizeof(IrLabel), 0),
  };

  // entry: push ret_site + push col, then branch to entrance rule (spec: peg_ir_emit_call)
  IrLabel final_ret = peg_ir_emit_call(&ctx, cl->scoped_rules[0].scoped_rule_name);

  for (int32_t i = 0; i < rule_size; i++) {
    ScopedRule* sr = &cl->scoped_rules[i];
    irwriter_rawf(w, "\n%s:\n", sr->scoped_rule_name);

    int32_t tag_size = symtab_count(&sr->tags);
    ctx.tag_bit_offset = (int64_t)sr->tag_bit_offset;

    IrLabel done_bb = irwriter_label(w);
    IrLabel fail_bb = irwriter_label(w);
    IrLabel material_parse = irwriter_label(w);

    if (memoize_mode == MEMO_NONE) {
      irwriter_br(w, material_parse);
    } else if (memoize_mode == MEMO_SHARED) {
      IrVal col_val = irwriter_load(w, "i64", col_ptr);
      IrVal row_off = irwriter_binop(w, "mul", "i64", col_val, irwriter_imm_int(w, (int)sizeof_col));
      IrVal slot_byte = irwriter_binop(
          w, "add", "i64", row_off, irwriter_imm_int(w, (int)(cl->bits_bucket_size * 8 + (int64_t)sr->slot_index * 4)));
      irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%r%d, i64 %%r%d\n", irwriter_next_reg(w), (int)peg_table,
                    (int)slot_byte);
      IrVal slot_ptr = (IrVal)(irwriter_next_reg(w) - 1);
      IrVal slot_val = irwriter_load(w, "i32", slot_ptr);
      IrVal is_cached = irwriter_icmp(w, "ne", "i32", slot_val, irwriter_imm_int(w, -1));

      uint64_t seg_off = sr->segment_index * 8;
      IrVal bt = irwriter_call_retf(w, "i1", "bit_test", "ptr %%r%d, i64 %%r%d, i64 %lld, i64 %llu, i64 %llu",
                                    (int)peg_table, (int)col_val, (long long)sizeof_col, (unsigned long long)seg_off,
                                    (unsigned long long)sr->rule_bit_mask);
      IrLabel bit_ok = irwriter_label(w);
      irwriter_br_cond(w, bt, bit_ok, fail_bb);

      irwriter_bb_at(w, bit_ok);
      IrLabel fast_ret = irwriter_label(w);
      irwriter_br_cond(w, is_cached, fast_ret, material_parse);
      irwriter_bb_at(w, fast_ret);
      IrVal cached = irwriter_sext(w, "i32", slot_val, "i64");
      irwriter_store(w, "i64", cached, parsed_tokens);
      irwriter_store(w, "i64", irwriter_binop(w, "add", "i64", col_val, cached), col_ptr);
      irwriter_br(w, done_bb);
    } else {
      // naive: three-way check on slot value
      IrVal col_val = irwriter_load(w, "i64", col_ptr);
      IrVal row_off = irwriter_binop(w, "mul", "i64", col_val, irwriter_imm_int(w, (int)sizeof_col));
      IrVal slot_byte = irwriter_binop(
          w, "add", "i64", row_off, irwriter_imm_int(w, (int)(cl->bits_bucket_size * 8 + (int64_t)sr->slot_index * 4)));
      irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%r%d, i64 %%r%d\n", irwriter_next_reg(w), (int)peg_table,
                    (int)slot_byte);
      IrVal slot_ptr = (IrVal)(irwriter_next_reg(w) - 1);
      IrVal slot_val = irwriter_load(w, "i32", slot_ptr);

      // slot >= 0 -> fast_ret (cached success)
      IrVal is_success = irwriter_icmp(w, "sge", "i32", slot_val, irwriter_imm_int(w, 0));
      IrLabel fast_ret = irwriter_label(w);
      IrLabel not_success = irwriter_label(w);
      irwriter_br_cond(w, is_success, fast_ret, not_success);

      irwriter_bb_at(w, fast_ret);
      IrVal cached = irwriter_sext(w, "i32", slot_val, "i64");
      irwriter_store(w, "i64", cached, parsed_tokens);
      irwriter_store(w, "i64", irwriter_binop(w, "add", "i64", col_val, cached), col_ptr);
      irwriter_br(w, done_bb);

      // slot == -2 -> fail_bb (cached failure), slot == -1 -> material_parse (unknown)
      irwriter_bb_at(w, not_success);
      IrVal is_unknown = irwriter_icmp(w, "eq", "i32", slot_val, irwriter_imm_int(w, -1));
      irwriter_br_cond(w, is_unknown, material_parse, fail_bb);
    }

    irwriter_bb_at(w, material_parse);

    if (tag_size > 0) {
      irwriter_store(w, "i64", irwriter_imm_int(w, 0), tag_bits);
    }

    IrLabel parse_fail = irwriter_label(w);
    IrLabel parse_success = irwriter_label(w);

    peg_ir_emit_parse(&ctx, &sr->body, parse_fail);
    irwriter_br(w, parse_success);

    irwriter_bb_at(w, parse_success);
    IrVal start_col = irwriter_call_retf(w, "i64", "top", "ptr %%r%d", (int)stack_ptr);
    IrVal cur_col = irwriter_load(w, "i64", col_ptr);
    IrVal pt = irwriter_binop(w, "sub", "i64", cur_col, start_col);
    irwriter_store(w, "i64", pt, parsed_tokens);

    if (tag_size > 0) {
      IrVal bo = irwriter_binop(w, "mul", "i64", start_col, irwriter_imm_int(w, (int)sizeof_col));
      IrVal tbo = irwriter_binop(w, "add", "i64", bo, irwriter_imm_int(w, (int)(sr->tag_bit_index * 8)));
      irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%r%d, i64 %%r%d\n", irwriter_next_reg(w), (int)peg_table,
                    (int)tbo);
      IrVal bp = (IrVal)(irwriter_next_reg(w) - 1);
      IrVal old = irwriter_load(w, "i64", bp);
      irwriter_rawf(w, "  %%r%d = and i64 %%r%d, %llu\n", irwriter_next_reg(w), (int)old,
                    (unsigned long long)~sr->tag_bit_mask);
      IrVal cleared = (IrVal)(irwriter_next_reg(w) - 1);
      IrVal combined = irwriter_binop(w, "or", "i64", cleared, irwriter_load(w, "i64", tag_bits));
      irwriter_store(w, "i64", combined, bp);
    }
    if (memoize_mode == MEMO_SHARED) {
      IrVal pt2 = irwriter_load(w, "i64", parsed_tokens);
      IrVal so = irwriter_binop(w, "mul", "i64", start_col, irwriter_imm_int(w, (int)sizeof_col));
      IrVal sbo = irwriter_binop(w, "add", "i64", so,
                                 irwriter_imm_int(w, (int)(cl->bits_bucket_size * 8 + (int64_t)sr->slot_index * 4)));
      irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%r%d, i64 %%r%d\n", irwriter_next_reg(w), (int)peg_table,
                    (int)sbo);
      IrVal wp = (IrVal)(irwriter_next_reg(w) - 1);
      irwriter_rawf(w, "  %%r%d = trunc i64 %%r%d to i32\n", irwriter_next_reg(w), (int)pt2);
      irwriter_store(w, "i32", (IrVal)(irwriter_next_reg(w) - 1), wp);
      irwriter_call_void_fmtf(w, "bit_exclude", "ptr %%r%d, i64 %%r%d, i64 %lld, i64 %llu, i64 %llu, i64 %llu",
                              (int)peg_table, (int)start_col, (long long)sizeof_col,
                              (unsigned long long)(sr->segment_index * 8), (unsigned long long)sr->segment_mask,
                              (unsigned long long)sr->rule_bit_mask);
    } else if (memoize_mode == MEMO_NAIVE) {
      IrVal pt2 = irwriter_load(w, "i64", parsed_tokens);
      IrVal so = irwriter_binop(w, "mul", "i64", start_col, irwriter_imm_int(w, (int)sizeof_col));
      IrVal sbo = irwriter_binop(w, "add", "i64", so,
                                 irwriter_imm_int(w, (int)(cl->bits_bucket_size * 8 + (int64_t)sr->slot_index * 4)));
      irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%r%d, i64 %%r%d\n", irwriter_next_reg(w), (int)peg_table,
                    (int)sbo);
      IrVal wp = (IrVal)(irwriter_next_reg(w) - 1);
      irwriter_rawf(w, "  %%r%d = trunc i64 %%r%d to i32\n", irwriter_next_reg(w), (int)pt2);
      irwriter_store(w, "i32", (IrVal)(irwriter_next_reg(w) - 1), wp);
    }
    irwriter_br(w, done_bb);

    // parse_fail: restore col, deny memoize, then branch to fail_bb
    irwriter_bb_at(w, parse_fail);
    IrVal fail_col = irwriter_call_retf(w, "i64", "top", "ptr %%r%d", (int)stack_ptr);
    irwriter_store(w, "i64", fail_col, col_ptr);
    if (memoize_mode == MEMO_SHARED) {
      irwriter_call_void_fmtf(w, "bit_deny", "ptr %%r%d, i64 %%r%d, i64 %lld, i64 %llu, i64 %llu", (int)peg_table,
                              (int)fail_col, (long long)sizeof_col, (unsigned long long)(sr->segment_index * 8),
                              (unsigned long long)sr->rule_bit_mask);
    } else if (memoize_mode == MEMO_NAIVE) {
      // cache failure as -2 in the slot
      IrVal so = irwriter_binop(w, "mul", "i64", fail_col, irwriter_imm_int(w, (int)sizeof_col));
      IrVal sbo = irwriter_binop(w, "add", "i64", so,
                                 irwriter_imm_int(w, (int)(cl->bits_bucket_size * 8 + (int64_t)sr->slot_index * 4)));
      irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%r%d, i64 %%r%d\n", irwriter_next_reg(w), (int)peg_table,
                    (int)sbo);
      IrVal wp = (IrVal)(irwriter_next_reg(w) - 1);
      irwriter_store(w, "i32", irwriter_imm_int(w, -2), wp);
    }
    irwriter_br(w, fail_bb);

    // fail_bb: set parsed_tokens = -1, branch to done_bb
    irwriter_bb_at(w, fail_bb);
    irwriter_store(w, "i64", irwriter_imm_int(w, -1), parsed_tokens);
    irwriter_br(w, done_bb);

    irwriter_bb_at(w, done_bb);
    peg_ir_emit_ret(&ctx);
  }

  // final_ret: landing pad after entrance rule returns via indirectbr
  irwriter_bb_at(w, final_ret);
  IrVal fr = irwriter_load(w, "i64", parse_result);
  IrVal fc = irwriter_load(w, "i64", col_ptr);
  IrVal r0 = irwriter_insertvalue(w, "{i64, i64}", -1, "i64", fr, 0);
  IrVal r1 = irwriter_insertvalue(w, "{i64, i64}", r0, "i64", fc, 1);
  irwriter_ret(w, "{i64, i64}", r1);
  irwriter_define_end(w);
  darray_del(ctx.ret_labels);
  free(fn_name);
}

// ============================================================
// Cleanup
// ============================================================

static void _free_scoped_unit(ScopedUnit* su) {
  switch (su->kind) {
  case SCOPED_UNIT_SEQ:
  case SCOPED_UNIT_BRANCHES:
    if (su->as.children) {
      for (size_t i = 0; i < darray_size(su->as.children); i++) {
        _free_scoped_unit(&su->as.children[i]);
      }
      darray_del(su->as.children);
    }
    break;
  case SCOPED_UNIT_MAYBE:
    if (su->as.base) {
      _free_scoped_unit(su->as.base);
      free(su->as.base);
    }
    break;
  case SCOPED_UNIT_STAR:
  case SCOPED_UNIT_PLUS:
    if (su->as.interlace.lhs) {
      _free_scoped_unit(su->as.interlace.lhs);
      free(su->as.interlace.lhs);
    }
    if (su->as.interlace.rhs) {
      _free_scoped_unit(su->as.interlace.rhs);
      free(su->as.interlace.rhs);
    }
    break;
  default:
    break;
  }
}

static void _free_closure(ScopeClosure* cl) {
  for (size_t i = 0; i < darray_size(cl->scoped_rules); i++) {
    _free_scoped_unit(&cl->scoped_rules[i].body);
    symtab_free(&cl->scoped_rules[i].tags);
    if (cl->scoped_rules[i].first_set) {
      bitset_del(cl->scoped_rules[i].first_set);
    }
    if (cl->scoped_rules[i].last_set) {
      bitset_del(cl->scoped_rules[i].last_set);
    }
  }
  darray_del(cl->scoped_rules);
  symtab_free(&cl->scoped_rule_names);
}

// ============================================================
// Entry point
// ============================================================

void peg_gen(PegGenInput* input, HeaderWriter* hw, IrWriter* w, int memoize_mode, const char* prefix) {
  int32_t rule_size = (int32_t)darray_size(input->rules);
  if (rule_size == 0) {
    return;
  }

  RuleLookup lu = _build_rule_lookup(input);

  if (input->verbose) {
    fprintf(stderr, "  [peg] gathering scope closures\n");
  }

  ScopeClosure* closures = darray_new(sizeof(ScopeClosure), 0);
  for (int32_t i = 0; i < rule_size; i++) {
    if (input->rules[i].scope_id >= 0) {
      ScopeClosure cl = _gather_scope_closures(&lu, &input->rules[i]);
      darray_push(closures, cl);
    }
  }
  int32_t closure_size = (int32_t)darray_size(closures);

  for (int32_t c = 0; c < closure_size; c++) {
    _alloc_tags(&lu, &closures[c]);
  }

  for (int32_t c = 0; c < closure_size; c++) {
    if (input->verbose) {
      fprintf(stderr, "  [peg] analyzing scope '%s'\n", closures[c].scope_name);
    }
    _analyze_rules(&closures[c]);
    if (memoize_mode == MEMO_SHARED) {
      _alloc_slot_bits(&closures[c]);
      _alloc_shared_tag_bits(&closures[c]);
    } else {
      _alloc_naive_slots(&closures[c]);
      _alloc_tag_bits(&closures[c]);
    }
  }

  peg_ir_emit_helpers(w);
  if (memoize_mode == MEMO_SHARED) {
    peg_ir_emit_bit_helpers(w);
  }
  for (int32_t c = 0; c < closure_size; c++) {
    if (input->verbose) {
      fprintf(stderr, "  [peg] generating IR for scope '%s'\n", closures[c].scope_name);
    }
    _gen_scope_ir(w, &closures[c], memoize_mode);
  }

  RuleScopeMap scope_map = _build_scope_map(input, closures, closure_size);
  _gen_header(input, &lu, hw, closures, closure_size, &scope_map, prefix);
  _free_scope_map(&scope_map);

  for (int32_t c = 0; c < closure_size; c++) {
    _free_closure(&closures[c]);
  }
  darray_del(closures);
  _free_rule_lookup(&lu);
}
