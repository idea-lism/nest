// specs/peg_analyze.md
#include "bitset.h"
#include "coloring.h"
#include "darray.h"
#include "graph.h"
#include "peg.h"
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

static uint64_t _tag_mask(int32_t bit_size, uint64_t offset) {
  if (bit_size <= 0 || offset >= 64) {
    return 0;
  }
  int32_t available_bits = 64 - (int32_t)offset;
  if (bit_size >= available_bits) {
    return UINT64_MAX << offset;
  }
  return ((1ULL << bit_size) - 1) << offset;
}

// ============================================================
// O(1) rule lookup by global_id
// ============================================================

typedef struct {
  PegAnalyzeInput* input;
  PegRule** rule_by_gid;
  int32_t rule_count;
} RuleLookup;

static bool _rule_gid_index(RuleLookup* lu, int32_t global_id, int32_t* out_idx) {
  int32_t idx = global_id - lu->input->rule_names.start_num;
  if (idx >= 0 && idx < lu->rule_count) {
    *out_idx = idx;
    return true;
  }
  return false;
}

static RuleLookup _build_rule_lookup(PegAnalyzeInput* input) {
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

static bool _is_scope_term(PegAnalyzeInput* input, int32_t id) {
  return id < symtab_count(&input->scope_names) + input->scope_names.start_num;
}

static const char* _term_name(PegAnalyzeInput* input, int32_t id) {
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
      gctx->closure->scoped_rules[idx].scoped_rule_name = (const char*)1;
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
  cl.source_line = scope_rule->source_line;
  cl.source_col = scope_rule->source_col;
  symtab_init(&cl.scoped_rule_names, 0);
  cl.scoped_rules = darray_new(sizeof(ScopedRule), 0);

  GatherCtx gctx = {.lu = lu, .closure = &cl, .multiplier_num = 0};

  int32_t scope_idx = _intern_rule(&cl, cl.scope_name, cl.scope_name, scope_rule->global_id);
  cl.scoped_rules[scope_idx].scoped_rule_name = (const char*)1;
  ScopedUnit scope_body = _breakdown(&gctx, &scope_rule->body, cl.scope_name);
  cl.scoped_rules[scope_idx].body = scope_body;

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

static void _alloc_shared_tag_bits(ScopeClosure* cl) {
  int32_t rule_size = (int32_t)darray_size(cl->scoped_rules);
  int32_t segment_size = (int32_t)cl->slots_size;

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

  int64_t next_bucket = segment_size;
  for (int32_t seg = 0; seg < segment_size; seg++) {
    int32_t max_tags = seg_max_tags[seg];
    if (max_tags == 0) {
      continue;
    }
    int32_t remaining = 64 - seg_used_bits[seg];
    uint64_t tag_bit_index;
    uint64_t tag_bit_offset;
    if (max_tags <= remaining) {
      tag_bit_index = (uint64_t)seg;
      tag_bit_offset = (uint64_t)seg_used_bits[seg];
    } else {
      tag_bit_index = (uint64_t)next_bucket;
      tag_bit_offset = 0;
      next_bucket++;
    }
    uint64_t tag_bit_mask = _tag_mask(max_tags, tag_bit_offset);
    for (int32_t i = 0; i < rule_size; i++) {
      if ((int32_t)cl->scoped_rules[i].segment_index == seg) {
        cl->scoped_rules[i].tag_bit_index = tag_bit_index;
        cl->scoped_rules[i].tag_bit_offset = tag_bit_offset;
        cl->scoped_rules[i].tag_bit_mask = tag_bit_mask;
      }
    }
  }
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
// Node field layout computation
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

static ScopedRule* _find_scoped_rule(ScopeClosure* cl, int32_t id) {
  if (id < 0) {
    return NULL;
  }
  return &cl->scoped_rules[id - cl->scoped_rule_names.start_num];
}

static int32_t _slot_row(ScopeClosure* cl, ScopedRule* sr) {
  return (int32_t)(cl->bits_bucket_size * 2 + (int64_t)sr->slot_index);
}

static void _build_child_fields(PegAnalyzeInput* input, ScopeClosure* cl, PegUnit* children, int32_t child_size,
                                FieldDedup* fd, const char* parent_rule_name, int32_t* multiplier_num_ptr,
                                NodeFields* out) {
  int64_t col_size_in_i32 = cl->bits_bucket_size * 2 + cl->slots_size;
  (void)col_size_in_i32;

  for (int32_t i = 0; i < child_size; i++) {
    PegUnit* u = &children[i];
    bool is_link = (u->multiplier == '*' || u->multiplier == '+');

    if (u->kind == PEG_TERM) {
      char* san = _sanitize_field_name(_term_name(input, u->id));
      char* dedup = _field_dedup_next(fd, san);
      bool is_scope = _is_scope_term(input, u->id);
      NodeField nf = {
          .name = dedup,
          .is_link = is_link,
          .is_scope = is_scope,
          .ref_row = 0,
          .rhs_row = -1,
          .advance = NODE_ADVANCE_ONE,
          .advance_slot_row = 0,
      };
      if (is_link) {
        (*multiplier_num_ptr)++;
        ScopedRule* wrapper = _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s$%d", cl->scope_name,
                                                                  parent_rule_name, *multiplier_num_ptr));
        nf.ref_row = wrapper ? _slot_row(cl, wrapper) : 0;
        nf.advance = NODE_ADVANCE_NONE;
      }
      darray_push(*out, nf);
      free(san);
    } else if (u->kind == PEG_CALL) {
      const char* callee_name = symtab_get(&input->rule_names, u->id);
      char* dedup = _field_dedup_next(fd, callee_name);
      NodeField nf = {
          .name = dedup,
          .is_link = is_link,
          .is_scope = false,
          .ref_row = 0,
          .rhs_row = -1,
          .advance = NODE_ADVANCE_NONE,
          .advance_slot_row = 0,
      };
      if (is_link) {
        (*multiplier_num_ptr)++;
        ScopedRule* wrapper = _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s$%d", cl->scope_name,
                                                                  parent_rule_name, *multiplier_num_ptr));
        nf.ref_row = wrapper ? _slot_row(cl, wrapper) : 0;
        if (u->interlace_rhs_kind == PEG_CALL) {
          const char* rhs_name = symtab_get(&input->rule_names, u->interlace_rhs_id);
          ScopedRule* rhs_sr =
              _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s", cl->scope_name, rhs_name));
          if (rhs_sr) {
            nf.rhs_row = _slot_row(cl, rhs_sr);
          }
        }
      } else {
        ScopedRule* callee_sr =
            _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s", cl->scope_name, callee_name));
        int32_t callee_row = callee_sr ? _slot_row(cl, callee_sr) : 0;
        nf.ref_row = callee_row;
        nf.advance = NODE_ADVANCE_SLOT;
        nf.advance_slot_row = callee_row;
      }
      darray_push(*out, nf);
    } else if (u->kind == PEG_BRANCHES) {
      for (size_t j = 0; j < darray_size(u->children); j++) {
        PegUnit* branch = &u->children[j];
        if (branch->kind == PEG_TERM) {
          char* san = _sanitize_field_name(_term_name(input, branch->id));
          char* dedup = _field_dedup_next(fd, san);
          NodeField nf = {
              .name = dedup,
              .is_link = false,
              .is_scope = _is_scope_term(input, branch->id),
              .ref_row = 0,
              .rhs_row = -1,
              .advance = NODE_ADVANCE_NONE,
              .advance_slot_row = 0,
          };
          darray_push(*out, nf);
          free(san);
        } else if (branch->kind == PEG_CALL) {
          const char* callee_name = symtab_get(&input->rule_names, branch->id);
          char* dedup = _field_dedup_next(fd, callee_name);
          ScopedRule* callee_sr =
              _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s", cl->scope_name, callee_name));
          int32_t callee_row = callee_sr ? _slot_row(cl, callee_sr) : 0;
          NodeField nf = {
              .name = dedup,
              .is_link = false,
              .is_scope = false,
              .ref_row = callee_row,
              .rhs_row = -1,
              .advance = NODE_ADVANCE_NONE,
              .advance_slot_row = 0,
          };
          darray_push(*out, nf);
        } else if (branch->kind == PEG_SEQ) {
          for (size_t k = 0; k < darray_size(branch->children); k++) {
            PegUnit* bc = &branch->children[k];
            if (bc->kind == PEG_TERM) {
              char* san = _sanitize_field_name(_term_name(input, bc->id));
              char* dedup = _field_dedup_next(fd, san);
              NodeField nf = {
                  .name = dedup,
                  .is_link = false,
                  .is_scope = _is_scope_term(input, bc->id),
                  .ref_row = 0,
                  .rhs_row = -1,
                  .advance = NODE_ADVANCE_NONE,
                  .advance_slot_row = 0,
              };
              darray_push(*out, nf);
              free(san);
            } else if (bc->kind == PEG_CALL) {
              const char* cn = symtab_get(&input->rule_names, bc->id);
              char* dedup = _field_dedup_next(fd, cn);
              ScopedRule* csr =
                  _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s", cl->scope_name, cn));
              int32_t crow = csr ? _slot_row(cl, csr) : 0;
              NodeField nf = {
                  .name = dedup,
                  .is_link = false,
                  .is_scope = false,
                  .ref_row = crow,
                  .rhs_row = -1,
                  .advance = NODE_ADVANCE_NONE,
                  .advance_slot_row = 0,
              };
              darray_push(*out, nf);
            }
          }
        }
      }
      // advance past the branch via wrapper rule's slot
      (*multiplier_num_ptr)++;
      ScopedRule* wrapper = _find_scoped_rule(
          cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s$%d", cl->scope_name, parent_rule_name, *multiplier_num_ptr));
      if (wrapper && darray_size(*out) > 0) {
        // attach advance to the last field added by this branch group
        // (the branch as a whole advances past all alternatives)
        NodeField* last = &(*out)[darray_size(*out) - 1];
        if (last->advance == NODE_ADVANCE_NONE) {
          last->advance = NODE_ADVANCE_SLOT;
          last->advance_slot_row = _slot_row(cl, wrapper);
        }
      }
    }
  }
}

static void _build_node_fields(PegAnalyzeInput* input, RuleLookup* lu, ScopeClosure* cl) {
  const char* rule_name = cl->scope_name;

  for (size_t i = 0; i < darray_size(cl->scoped_rules); i++) {
    ScopedRule* sr = &cl->scoped_rules[i];
    if (sr->original_global_id < 0) {
      sr->node_fields = NULL;
      continue;
    }
    PegRule* orig = _lookup_rule(lu, sr->original_global_id);
    if (!orig) {
      sr->node_fields = NULL;
      continue;
    }

    // determine the rule_name for this scoped rule (strip scope prefix)
    const char* orig_name = symtab_get(&input->rule_names, sr->original_global_id);
    (void)rule_name;

    sr->node_fields = darray_new(sizeof(NodeField), 0);
    FieldDedup fd;
    _field_dedup_init(&fd);
    int32_t multiplier_num = 0;

    if (orig->body.kind == PEG_SEQ) {
      _build_child_fields(input, cl, orig->body.children, (int32_t)darray_size(orig->body.children), &fd, orig_name,
                          &multiplier_num, &sr->node_fields);
    } else if (orig->body.kind == PEG_BRANCHES) {
      for (size_t j = 0; j < darray_size(orig->body.children); j++) {
        PegUnit* branch = &orig->body.children[j];
        multiplier_num = 0;
        if (branch->kind == PEG_SEQ) {
          _build_child_fields(input, cl, branch->children, (int32_t)darray_size(branch->children), &fd, orig_name,
                              &multiplier_num, &sr->node_fields);
        } else if (branch->kind == PEG_TERM || branch->kind == PEG_CALL) {
          _build_child_fields(input, cl, branch, 1, &fd, orig_name, &multiplier_num, &sr->node_fields);
        }
      }
    } else if (orig->body.kind == PEG_TERM) {
      bool is_link = (orig->body.multiplier == '*' || orig->body.multiplier == '+');
      char* san = _sanitize_field_name(_term_name(input, orig->body.id));
      bool is_scope = _is_scope_term(input, orig->body.id);
      NodeField nf = {
          .name = san,
          .is_link = is_link,
          .is_scope = is_scope,
          .ref_row = 0,
          .rhs_row = -1,
          .advance = is_link ? NODE_ADVANCE_NONE : NODE_ADVANCE_ONE,
          .advance_slot_row = 0,
      };
      if (is_link) {
        ScopedRule* wrapper =
            _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s$%d", cl->scope_name, orig_name, 1));
        nf.ref_row = wrapper ? _slot_row(cl, wrapper) : 0;
      }
      darray_push(sr->node_fields, nf);
    } else if (orig->body.kind == PEG_CALL) {
      bool is_link = (orig->body.multiplier == '*' || orig->body.multiplier == '+');
      const char* fn = symtab_get(&input->rule_names, orig->body.id);
      NodeField nf = {
          .name = strdup(fn),
          .is_link = is_link,
          .is_scope = false,
          .ref_row = 0,
          .rhs_row = -1,
          .advance = NODE_ADVANCE_NONE,
          .advance_slot_row = 0,
      };
      if (is_link) {
        ScopedRule* wrapper =
            _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s$%d", cl->scope_name, orig_name, 1));
        nf.ref_row = wrapper ? _slot_row(cl, wrapper) : 0;
      } else {
        ScopedRule* callee_sr =
            _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s", cl->scope_name, fn));
        int32_t callee_row = callee_sr ? _slot_row(cl, callee_sr) : 0;
        nf.ref_row = callee_row;
        nf.advance = NODE_ADVANCE_SLOT;
        nf.advance_slot_row = callee_row;
      }
      darray_push(sr->node_fields, nf);
    }

    _field_dedup_free(&fd);
  }
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

static void _free_node_fields(NodeFields fields) {
  if (!fields) {
    return;
  }
  for (size_t i = 0; i < darray_size(fields); i++) {
    free(fields[i].name);
  }
  darray_del(fields);
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
    _free_node_fields(cl->scoped_rules[i].node_fields);
  }
  darray_del(cl->scoped_rules);
  symtab_free(&cl->scoped_rule_names);
}

// ============================================================
// Public API
// ============================================================

PegGenInput peg_analyze(PegAnalyzeInput* input, int memoize_mode, const char* prefix) {
  PegGenInput result = {
      .tokens = input->tokens,
      .scope_names = input->scope_names,
      .rule_names = input->rule_names,
      .memoize_mode = memoize_mode,
      .prefix = prefix,
      .verbose = input->verbose,
  };

  int32_t rule_size = (int32_t)darray_size(input->rules);
  if (rule_size == 0) {
    result.scope_closures = darray_new(sizeof(ScopeClosure), 0);
    return result;
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
    if (memoize_mode == MEMOIZE_SHARED) {
      _alloc_slot_bits(&closures[c]);
      _alloc_shared_tag_bits(&closures[c]);
    } else {
      _alloc_naive_slots(&closures[c]);
      _alloc_tag_bits(&closures[c]);
    }
  }

  for (int32_t c = 0; c < closure_size; c++) {
    _build_node_fields(input, &lu, &closures[c]);
  }

  _free_rule_lookup(&lu);
  result.scope_closures = closures;
  return result;
}

void peg_analyze_free(PegGenInput* result) {
  if (!result->scope_closures) {
    return;
  }
  int32_t closure_size = (int32_t)darray_size(result->scope_closures);
  for (int32_t c = 0; c < closure_size; c++) {
    _free_closure(&result->scope_closures[c]);
  }
  darray_del(result->scope_closures);
  result->scope_closures = NULL;
}
