// specs/peg.md — PEG packrat parser generator
#include "peg.h"
#include "bitset.h"
#include "coloring.h"
#include "darray.h"
#include "graph.h"
#include "peg_ir.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================
// Scope closure: gather rules reachable from a scope
// ============================================================

static void _walk_unit(PegGenInput* input, PegUnit* unit, Symtab* defined, int32_t** visited);

static void _walk_call(PegGenInput* input, int32_t global_id, Symtab* defined, int32_t** visited) {
  for (int32_t i = 0; i < (int32_t)darray_size(*visited); i++) {
    if ((*visited)[i] == global_id) {
      return;
    }
  }
  darray_push(*visited, global_id);

  PegRule* rule = NULL;
  for (int32_t i = 0; i < (int32_t)darray_size(input->rules); i++) {
    if (input->rules[i].global_id == global_id) {
      rule = &input->rules[i];
      break;
    }
  }
  if (!rule) {
    return;
  }
  if (rule->scope_id >= 0) {
    return;
  }

  const char* name = symtab_get(&input->rule_names, global_id);
  symtab_intern(defined, name);
  _walk_unit(input, &rule->body, defined, visited);
}

static void _walk_unit(PegGenInput* input, PegUnit* unit, Symtab* defined, int32_t** visited) {
  if (unit->kind == PEG_CALL) {
    _walk_call(input, unit->id, defined, visited);
  }
  if (unit->children) {
    for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
      _walk_unit(input, &unit->children[i], defined, visited);
    }
  }
  if (unit->interlace_rhs_kind == PEG_CALL) {
    _walk_call(input, unit->interlace_rhs_id, defined, visited);
  }
}

static ScopeClosure* _gather_scope_closures(PegGenInput* input, int32_t* out_n) {
  ScopeClosure* out = darray_new(sizeof(ScopeClosure), 0);

  for (int32_t r = 0; r < (int32_t)darray_size(input->rules); r++) {
    PegRule* pr = &input->rules[r];
    if (pr->scope_id < 0) {
      continue;
    }
    const char* scope_name = symtab_get(&input->rule_names, pr->global_id);

    ScopeClosure sc = {0};
    sc.scope_name = scope_name;
    sc.scope_id = pr->scope_id;
    symtab_init(&sc.defined_rules, 0);
    sc.rules = NULL;
    sc.root_ids = NULL;

    symtab_intern(&sc.defined_rules, scope_name);
    int32_t* vis = darray_new(sizeof(int32_t), 0);
    darray_push(vis, pr->global_id);
    _walk_unit(input, &pr->body, &sc.defined_rules, &vis);
    darray_del(vis);

    darray_push(out, sc);
  }

  *out_n = (int32_t)darray_size(out);
  return out;
}

// ============================================================
// Rule breakdown: PegUnit -> flat ScopedRule list
// ============================================================

static PegRule* _find_rule(PegGenInput* input, int32_t global_id) {
  for (int32_t i = 0; i < (int32_t)darray_size(input->rules); i++) {
    if (input->rules[i].global_id == global_id) {
      return &input->rules[i];
    }
  }
  return NULL;
}

static int32_t _add_rule(ScopeClosure* sc, ScopedRule sr) {
  int32_t id = (int32_t)darray_size(sc->rules);
  sr.scoped_rule_id = (uint32_t)id;
  darray_push(sc->rules, sr);
  return id;
}

static int32_t _breakdown_unit(PegGenInput* input, ScopeClosure* sc, PegUnit* unit, const char* prefix, int32_t idx);

static char* _sub_name(const char* prefix, int32_t idx) {
  if (idx < 0) {
    return NULL;
  }
  int len = snprintf(NULL, 0, "%s$%d", prefix, idx);
  char* s = malloc((size_t)len + 1);
  snprintf(s, (size_t)len + 1, "%s$%d", prefix, idx);
  return s;
}

static char* _rhs_name(const char* prefix) {
  int len = snprintf(NULL, 0, "%s$sep", prefix);
  char* s = malloc((size_t)len + 1);
  snprintf(s, (size_t)len + 1, "%s$sep", prefix);
  return s;
}

// Placeholder call target — will be fixed up after all rules are broken down
#define CALL_PLACEHOLDER (-999)

static int32_t _make_interlace_rhs(PegGenInput* input, ScopeClosure* sc, PegUnit* unit, const char* name) {
  ScopedRule rhs = {0};
  rhs.name = _rhs_name(name);
  if (unit->interlace_rhs_kind == PEG_TERM) {
    rhs.kind = SCOPED_RULE_KIND_TERM;
    rhs.as.term = unit->interlace_rhs_id;
  } else {
    const char* rn = symtab_get(&input->rule_names, unit->interlace_rhs_id);
    PegRule* cr = _find_rule(input, unit->interlace_rhs_id);
    if (cr && cr->scope_id >= 0) {
      rhs.kind = SCOPED_RULE_KIND_TERM;
      rhs.as.term = cr->scope_id;
    } else {
      int32_t sid = symtab_find(&sc->defined_rules, rn);
      rhs.kind = SCOPED_RULE_KIND_CALL;
      // store symtab ID as placeholder; will be fixed to root_ids[sid] later
      rhs.as.call = sid >= 0 ? sid : 0;
      rhs.as.call = -(sid + 1); // encode as negative to mark as unfixed
    }
  }
  return _add_rule(sc, rhs);
}

static int32_t _breakdown_unit(PegGenInput* input, ScopeClosure* sc, PegUnit* unit, const char* prefix, int32_t idx) {
  char* name = _sub_name(prefix, idx);
  const char* n = name ? name : prefix;

  int32_t result;
  switch (unit->kind) {
  case PEG_TERM: {
    ScopedRule sr = {.name = n, .kind = SCOPED_RULE_KIND_TERM, .as.term = unit->id, .multiplier = unit->multiplier};
    if (unit->interlace_rhs_kind) {
      sr.multiplier = 0;
      int32_t lhs = _add_rule(sc, sr);
      int32_t rhs = _make_interlace_rhs(input, sc, unit, n);
      ScopedRule j = {.name = n, .kind = SCOPED_RULE_KIND_JOIN, .multiplier = unit->multiplier};
      j.as.join.lhs = lhs;
      j.as.join.rhs = rhs;
      result = _add_rule(sc, j);
    } else {
      result = _add_rule(sc, sr);
    }
    break;
  }
  case PEG_CALL: {
    const char* callee = symtab_get(&input->rule_names, unit->id);
    PegRule* cr = _find_rule(input, unit->id);

    ScopedRule sr = {.name = n, .multiplier = unit->multiplier};
    if (cr && cr->scope_id >= 0) {
      sr.kind = SCOPED_RULE_KIND_TERM;
      sr.as.term = cr->scope_id;
    } else {
      int32_t sid = symtab_find(&sc->defined_rules, callee);
      sr.kind = SCOPED_RULE_KIND_CALL;
      sr.as.call = -(sid + 1); // encoded placeholder
    }

    if (unit->interlace_rhs_kind) {
      sr.multiplier = 0;
      int32_t lhs = _add_rule(sc, sr);
      int32_t rhs = _make_interlace_rhs(input, sc, unit, n);
      ScopedRule j = {.name = n, .kind = SCOPED_RULE_KIND_JOIN, .multiplier = unit->multiplier};
      j.as.join.lhs = lhs;
      j.as.join.rhs = rhs;
      result = _add_rule(sc, j);
    } else {
      result = _add_rule(sc, sr);
    }
    break;
  }
  case PEG_SEQ: {
    int32_t nc = (int32_t)darray_size(unit->children);
    int32_t* kids = darray_new(sizeof(int32_t), 0);
    for (int32_t i = 0; i < nc; i++) {
      int32_t cid = _breakdown_unit(input, sc, &unit->children[i], n, i);
      darray_push(kids, cid);
    }
    ScopedRule sr = {.name = n, .kind = SCOPED_RULE_KIND_SEQ, .multiplier = unit->multiplier};
    sr.as.seq = kids;
    result = _add_rule(sc, sr);
    break;
  }
  case PEG_BRANCHES: {
    int32_t nb = (int32_t)darray_size(unit->children);
    int32_t* bids = darray_new(sizeof(int32_t), 0);
    for (int32_t i = 0; i < nb; i++) {
      PegUnit* branch = &unit->children[i];
      PegUnit* actual = branch;
      if (branch->kind == PEG_SEQ && branch->children && darray_size(branch->children) == 1) {
        actual = &branch->children[0];
      }
      int32_t bid = _breakdown_unit(input, sc, actual, n, i);
      if (sc->rules[bid].kind == SCOPED_RULE_KIND_BRANCHES) {
        int32_t* wrap = darray_new(sizeof(int32_t), 0);
        darray_push(wrap, bid);
        char* wn = _sub_name(n, (int32_t)(i + 1000));
        ScopedRule wr = {.name = wn, .kind = SCOPED_RULE_KIND_SEQ};
        wr.as.seq = wrap;
        bid = _add_rule(sc, wr);
      }
      darray_push(bids, bid);
    }
    ScopedRule sr = {.name = n, .kind = SCOPED_RULE_KIND_BRANCHES, .multiplier = unit->multiplier};
    sr.as.branches = bids;
    result = _add_rule(sc, sr);
    break;
  }
  default:
    result = -1;
  }

  return result;
}

// Fix up call targets: replace encoded symtab IDs with actual root rule indices.
// Also wrap branch children that resolve to BRANCHES after lowering.
static void _fixup_calls(ScopeClosure* sc) {
  int32_t n = (int32_t)darray_size(sc->rules);
  for (int32_t i = 0; i < n; i++) {
    ScopedRule* r = &sc->rules[i];
    if (r->kind == SCOPED_RULE_KIND_CALL && r->as.call < 0) {
      int32_t sid = -(r->as.call + 1);
      if (sid >= 0 && sid < (int32_t)darray_size(sc->root_ids)) {
        r->as.call = sc->root_ids[sid];
      } else {
        r->as.call = 0;
      }
    }
  }

  // Second pass: wrap branch children that resolve to BRANCHES after lowering
  for (int32_t i = 0; i < (int32_t)darray_size(sc->rules); i++) {
    ScopedRule* r = &sc->rules[i];
    if (r->kind != SCOPED_RULE_KIND_BRANCHES) {
      continue;
    }
    int32_t nb = (int32_t)darray_size(r->as.branches);
    for (int32_t j = 0; j < nb; j++) {
      int32_t child_id = r->as.branches[j];
      ScopedRule* child = &sc->rules[child_id];
      // If child is a CALL, resolve to its target and check if target is BRANCHES
      int32_t target_id = child_id;
      if (child->kind == SCOPED_RULE_KIND_CALL) {
        target_id = child->as.call;
      }
      ScopedRule* target = &sc->rules[target_id];
      if (target->kind == SCOPED_RULE_KIND_BRANCHES && child->kind != SCOPED_RULE_KIND_SEQ) {
        // wrap in seq
        int32_t* wrap = darray_new(sizeof(int32_t), 0);
        darray_push(wrap, child_id);
        int wn_len = snprintf(NULL, 0, "%s$%d", r->name, j + 1000);
        char* wn = malloc((size_t)wn_len + 1);
        snprintf(wn, (size_t)wn_len + 1, "%s$%d", r->name, j + 1000);
        ScopedRule wr = {.name = wn, .kind = SCOPED_RULE_KIND_SEQ};
        wr.as.seq = wrap;
        int32_t wrap_id = _add_rule(sc, wr);
        // re-fetch r since darray may have realloc'd
        r = &sc->rules[i];
        r->as.branches[j] = wrap_id;
      }
    }
  }
}

static void _breakdown_rules(PegGenInput* input, ScopeClosure* closures, int32_t n) {
  for (int32_t ci = 0; ci < n; ci++) {
    ScopeClosure* sc = &closures[ci];
    sc->rules = darray_new(sizeof(ScopedRule), 0);
    sc->root_ids = darray_new(sizeof(int32_t), 0);

    int32_t nd = symtab_count(&sc->defined_rules);
    for (int32_t di = 0; di < nd; di++) {
      const char* rn = symtab_get(&sc->defined_rules, di);
      PegRule* pr = NULL;
      for (int32_t ri = 0; ri < (int32_t)darray_size(input->rules); ri++) {
        const char* rname = symtab_get(&input->rule_names, input->rules[ri].global_id);
        if (strcmp(rname, rn) == 0) {
          pr = &input->rules[ri];
          break;
        }
      }
      int32_t root_id = -1;
      if (pr) {
        root_id = _breakdown_unit(input, sc, &pr->body, rn, -1);
      }
      darray_push(sc->root_ids, root_id);
    }

    _fixup_calls(sc);
  }
}

// ============================================================
// Sort memoizables to front
// ============================================================

typedef struct {
  int32_t old;
  bool needs_memo;
} _MemoSort;

static int _memo_sort_cmp(const void* a, const void* b) {
  const _MemoSort* ma = a;
  const _MemoSort* mb = b;
  int ka = ma->needs_memo ? -1 : 1;
  int kb = mb->needs_memo ? -1 : 1;
  if (ka != kb) {
    return ka - kb;
  }
  return ma->old - mb->old;
}

static void _sort_memoizables(ScopeClosure* sc) {
  int32_t n = (int32_t)darray_size(sc->rules);
  if (n == 0) {
    return;
  }

  // 1. create new_to_old with {old, needs_memo}
  _MemoSort* new_to_old = malloc(n * sizeof(_MemoSort));
  for (int32_t i = 0; i < n; i++) {
    new_to_old[i].old = i;
    new_to_old[i].needs_memo = sc->rules[i].needs_memo;
  }

  // 2. sort by needs_memo ? -1 : 1, tiebreak by old index
  qsort(new_to_old, n, sizeof(_MemoSort), _memo_sort_cmp);

  // 3. create reverse mapping old_to_new
  int32_t* old_to_new = malloc(n * sizeof(int32_t));
  for (int32_t i = 0; i < n; i++) {
    old_to_new[new_to_old[i].old] = i;
  }

  // 4. create new_rules, rewrite call/join targets by old_to_new
  ScopedRule* new_rules = darray_new(sizeof(ScopedRule), 0);
  sc->memoizable_size = 0;
  for (int32_t i = 0; i < n; i++) {
    ScopedRule r = sc->rules[new_to_old[i].old];
    r.scoped_rule_id = (uint32_t)i;
    switch (r.kind) {
    case SCOPED_RULE_KIND_CALL:
      r.as.call = old_to_new[r.as.call];
      break;
    case SCOPED_RULE_KIND_JOIN:
      r.as.join.lhs = old_to_new[r.as.join.lhs];
      r.as.join.rhs = old_to_new[r.as.join.rhs];
      break;
    case SCOPED_RULE_KIND_SEQ:
      for (int32_t j = 0; j < (int32_t)darray_size(r.as.seq); j++) {
        r.as.seq[j] = old_to_new[r.as.seq[j]];
      }
      break;
    case SCOPED_RULE_KIND_BRANCHES:
      for (int32_t j = 0; j < (int32_t)darray_size(r.as.branches); j++) {
        r.as.branches[j] = old_to_new[r.as.branches[j]];
      }
      break;
    case SCOPED_RULE_KIND_TERM:
      break;
    }
    if (r.needs_memo) {
      sc->memoizable_size++;
    }
    darray_push(new_rules, r);
  }

  // remap root_ids
  for (int32_t i = 0; i < (int32_t)darray_size(sc->root_ids); i++) {
    if (sc->root_ids[i] >= 0) {
      sc->root_ids[i] = old_to_new[sc->root_ids[i]];
    }
  }

  // 5. replace rules with new_rules
  darray_del(sc->rules);
  sc->rules = new_rules;

  free(new_to_old);
  free(old_to_new);
}

// ============================================================
// Nullable / first_set / last_set
// ============================================================

static void _compute_nullable(ScopeClosure* sc) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (int32_t i = 0; i < (int32_t)darray_size(sc->rules); i++) {
      ScopedRule* r = &sc->rules[i];
      if (r->nullable) {
        continue;
      }
      bool v = false;
      if (r->multiplier == '?' || r->multiplier == '*') {
        v = true;
      } else {
        switch (r->kind) {
        case SCOPED_RULE_KIND_TERM:
          v = false;
          break;
        case SCOPED_RULE_KIND_CALL:
          v = sc->rules[r->as.call].nullable;
          break;
        case SCOPED_RULE_KIND_SEQ:
          v = true;
          for (int32_t j = 0; j < (int32_t)darray_size(r->as.seq); j++) {
            if (!sc->rules[r->as.seq[j]].nullable) {
              v = false;
              break;
            }
          }
          break;
        case SCOPED_RULE_KIND_BRANCHES:
          v = false;
          for (int32_t j = 0; j < (int32_t)darray_size(r->as.branches); j++) {
            if (sc->rules[r->as.branches[j]].nullable) {
              v = true;
              break;
            }
          }
          break;
        case SCOPED_RULE_KIND_JOIN:
          v = sc->rules[r->as.join.lhs].nullable;
          break;
        }
      }
      if (v) {
        r->nullable = true;
        changed = true;
      }
    }
  }
}

static void _merge_into(Bitset** dst, Bitset* src) {
  Bitset* m = bitset_or(*dst, src);
  bitset_del(*dst);
  *dst = m;
}

static void _compute_first_last(ScopeClosure* sc) {
  int32_t n = (int32_t)darray_size(sc->rules);
  for (int32_t i = 0; i < n; i++) {
    sc->rules[i].first_set = bitset_new();
    sc->rules[i].last_set = bitset_new();
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (int32_t i = 0; i < n; i++) {
      ScopedRule* r = &sc->rules[i];
      uint32_t fs = bitset_size(r->first_set);
      uint32_t ls = bitset_size(r->last_set);

      switch (r->kind) {
      case SCOPED_RULE_KIND_TERM:
        bitset_add_bit(r->first_set, (uint32_t)r->as.term);
        bitset_add_bit(r->last_set, (uint32_t)r->as.term);
        break;
      case SCOPED_RULE_KIND_CALL:
        _merge_into(&r->first_set, sc->rules[r->as.call].first_set);
        _merge_into(&r->last_set, sc->rules[r->as.call].last_set);
        break;
      case SCOPED_RULE_KIND_SEQ: {
        int32_t sn = (int32_t)darray_size(r->as.seq);
        for (int32_t j = 0; j < sn; j++) {
          _merge_into(&r->first_set, sc->rules[r->as.seq[j]].first_set);
          if (!sc->rules[r->as.seq[j]].nullable) {
            break;
          }
        }
        for (int32_t j = sn - 1; j >= 0; j--) {
          _merge_into(&r->last_set, sc->rules[r->as.seq[j]].last_set);
          if (!sc->rules[r->as.seq[j]].nullable) {
            break;
          }
        }
        break;
      }
      case SCOPED_RULE_KIND_BRANCHES:
        for (int32_t j = 0; j < (int32_t)darray_size(r->as.branches); j++) {
          _merge_into(&r->first_set, sc->rules[r->as.branches[j]].first_set);
          _merge_into(&r->last_set, sc->rules[r->as.branches[j]].last_set);
        }
        break;
      case SCOPED_RULE_KIND_JOIN:
        _merge_into(&r->first_set, sc->rules[r->as.join.lhs].first_set);
        _merge_into(&r->last_set, sc->rules[r->as.join.lhs].last_set);
        break;
      }

      if (bitset_size(r->first_set) != fs || bitset_size(r->last_set) != ls) {
        changed = true;
      }
    }
  }
}

static bool _exclusive(ScopedRule* a, ScopedRule* b) {
  if (a->nullable || b->nullable) {
    return false;
  }
  Bitset* fi = bitset_and(a->first_set, b->first_set);
  bool fd = (bitset_size(fi) == 0);
  bitset_del(fi);
  if (fd) {
    return true;
  }
  Bitset* li = bitset_and(a->last_set, b->last_set);
  bool ld = (bitset_size(li) == 0);
  bitset_del(li);
  return ld;
}

// ============================================================
// Coloring
// ============================================================

static void _assign_naive(ScopeClosure* sc) {
  int32_t n = (int32_t)darray_size(sc->rules);
  int32_t next_slot = 0;
  for (int32_t i = 0; i < n; i++) {
    if (!sc->rules[i].needs_memo) {
      continue;
    }
    sc->rules[i].slot_index = (uint32_t)next_slot++;
    sc->rules[i].segment_index = 0;
    sc->rules[i].segment_mask = 0;
    sc->rules[i].rule_bit_mask = 0;
  }
}

static void _assign_colored(ScopeClosure* sc, int32_t verbose) {
  int32_t n = (int32_t)darray_size(sc->rules);

  int32_t n_memo = 0;
  for (int32_t i = 0; i < n; i++) {
    if (sc->rules[i].needs_memo) {
      n_memo++;
    }
  }
  if (n_memo <= 1) {
    _assign_naive(sc);
    return;
  }

  int32_t* memo_to_rule = malloc(n_memo * sizeof(int32_t));
  int32_t mi = 0;
  for (int32_t i = 0; i < n; i++) {
    if (sc->rules[i].needs_memo) {
      memo_to_rule[mi++] = i;
    }
  }

  Graph* g = graph_new(n_memo);
  for (int32_t i = 0; i < n_memo; i++) {
    for (int32_t j = i + 1; j < n_memo; j++) {
      if (!_exclusive(&sc->rules[memo_to_rule[i]], &sc->rules[memo_to_rule[j]])) {
        graph_add_edge(g, i, j);
      }
    }
  }

  int32_t min_k = 1;
  int32_t* clique = graph_find_max_clique(g);
  if (clique) {
    min_k = clique[0];
    free(clique);
  }

  if (verbose) {
    fprintf(stderr, "  [peg] %s: clique lower bound = %d\n", sc->scope_name, min_k);
  }

  ColoringResult* cr = NULL;
  int32_t k;
  for (k = min_k; k <= n_memo; k++) {
    cr = coloring_solve(n_memo, graph_edges(g), graph_n_edges(g), k, 50000, 42);
    if (cr) {
      break;
    }
  }
  graph_del(g);

  if (!cr) {
    free(memo_to_rule);
    _assign_naive(sc);
    return;
  }

  int32_t sg_size = coloring_get_sg_size(cr);
  int32_t* sg_slot = malloc(sg_size * sizeof(int32_t));
  memset(sg_slot, -1, sg_size * sizeof(int32_t));
  int32_t next_slot = 0;

  for (int32_t i = 0; i < n_memo; i++) {
    int32_t ri = memo_to_rule[i];
    int32_t sg_id, seg_mask;
    coloring_get_segment_info(cr, i, &sg_id, &seg_mask);
    sc->rules[ri].segment_index = (uint32_t)sg_id;
    sc->rules[ri].rule_bit_mask = (uint32_t)seg_mask;
    sc->rules[ri].segment_mask = (uint32_t)seg_mask;
    if (sg_slot[sg_id] < 0) {
      sg_slot[sg_id] = next_slot++;
    }
    sc->rules[ri].slot_index = (uint32_t)sg_slot[sg_id];
  }

  free(sg_slot);
  free(memo_to_rule);
  coloring_result_del(cr);
}

// ============================================================
// Code generation: per-scope function with per-rule memoization
// ============================================================

static void _gen_scope(PegGenInput* input, ScopeClosure* sc, IrWriter* w, bool compress) {
  (void)input;
  int32_t n_rules = (int32_t)darray_size(sc->rules);
  if (n_rules == 0) {
    return;
  }

  int32_t n_sg = 0;
  int32_t n_slots = 0;
  if (compress) {
    for (int32_t i = 0; i < n_rules; i++) {
      if (!sc->rules[i].needs_memo) {
        continue;
      }
      if ((int32_t)sc->rules[i].segment_index + 1 > n_sg) {
        n_sg = (int32_t)sc->rules[i].segment_index + 1;
      }
      if ((int32_t)sc->rules[i].slot_index + 1 > n_slots) {
        n_slots = (int32_t)sc->rules[i].slot_index + 1;
      }
    }
  } else {
    for (int32_t i = 0; i < n_rules; i++) {
      if (sc->rules[i].needs_memo) {
        n_slots++;
      }
    }
  }

  int32_t col_sizeof = n_sg * 4 + n_slots * 4;
  if (col_sizeof == 0) {
    col_sizeof = 4;
  }

  const char* at[] = {"ptr", "ptr"};
  const char* an[] = {"tt", "stack_mem"};
  int fn_len = snprintf(NULL, 0, "parse_%s", sc->scope_name);
  char* fn = malloc((size_t)fn_len + 1);
  snprintf(fn, (size_t)fn_len + 1, "parse_%s", sc->scope_name);
  irwriter_define_start(w, fn, "void", 2, at, an);
  free(fn);

  irwriter_bb(w);

  irwriter_declare(w, "ptr", "malloc", "i64");
  irwriter_declare(w, "void", "free", "ptr");
  irwriter_declare(w, "void", "llvm.memset.p0.i64", "ptr, i8, i64, i1");

  // %tc = tt->current
  IrVal tc_pp = irwriter_next_reg(w);
  irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %s, i64 24\n", (int)tc_pp, "%tt");
  IrVal tc = irwriter_load(w, "ptr", tc_pp);

  IrVal tok_pp = irwriter_next_reg(w);
  irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr ", (int)tok_pp);
  irwriter_emit_val(w, tc);
  irwriter_rawf(w, ", i64 16\n");
  IrVal tokens = irwriter_load(w, "ptr", tok_pp);

  IrVal cnt_pp = irwriter_next_reg(w);
  irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr ", (int)cnt_pp);
  irwriter_emit_val(w, tokens);
  irwriter_rawf(w, ", i64 -4\n");
  IrVal n_tokens_i32 = irwriter_load(w, "i32", cnt_pp);
  IrVal n_tokens = irwriter_sext(w, "i32", n_tokens_i32, "i64");

  IrVal tbl_bytes = irwriter_binop(w, "mul", "i64", n_tokens, irwriter_imm_int(w, col_sizeof));
  IrVal tbl = irwriter_next_reg(w);
  irwriter_rawf(w, "  %%r%d = call ptr @malloc(i64 ", (int)tbl);
  irwriter_emit_val(w, tbl_bytes);
  irwriter_rawf(w, ")\n");

  irwriter_rawf(w, "  call void @llvm.memset.p0.i64(ptr ");
  irwriter_emit_val(w, tbl);
  irwriter_rawf(w, ", i8 -1, i64 ");
  irwriter_emit_val(w, tbl_bytes);
  irwriter_rawf(w, ", i1 false)\n");

  IrVal val_pp = irwriter_next_reg(w);
  irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr ", (int)val_pp);
  irwriter_emit_val(w, tc);
  irwriter_rawf(w, ", i64 8\n");
  irwriter_store(w, "ptr", tbl, val_pp);

  IrVal col_a = irwriter_alloca(w, "i64");
  irwriter_store(w, "i64", irwriter_imm_int(w, 0), col_a);
  IrVal sp_a = irwriter_alloca(w, "ptr");
  irwriter_store(w, "ptr", irwriter_imm(w, "%stack_mem"), sp_a);
  IrVal bp_a = irwriter_alloca(w, "ptr");
  irwriter_store(w, "ptr", irwriter_imm(w, "%stack_mem"), bp_a);
  IrVal ret_a = irwriter_alloca(w, "i32");
  IrVal fast_ret_a = irwriter_alloca(w, "ptr");

  PegIrCtx ctx = {
      .w = w,
      .tokens = tokens,
      .col_index = col_a,
      .stack = sp_a,
      .stack_bp = bp_a,
      .ret_val = ret_a,
      .fast_ret = fast_ret_a,
      .table = tbl,
      .n_tokens = n_tokens,
      .scope_name = sc->scope_name,
      .scoped_rule_names = &sc->defined_rules,
      .rules = sc->rules,
      .compress = compress,
      .col_sizeof = col_sizeof,
      .bits_offset = 0,
      .slots_offset = n_sg * 4,
      .n_seg_groups = n_sg,
      .n_slots = n_slots,
  };

  // find entry rule
  int32_t entry_id = 0;
  for (int32_t i = 0; i < n_rules; i++) {
    if (strcmp(sc->rules[i].name, sc->scope_name) == 0) {
      entry_id = i;
      break;
    }
  }

  IrLabel fail_bb = irwriter_label(w);
  IrLabel succ_bb = irwriter_label(w);
  ctx.fail_label = fail_bb;

  (void)peg_ir_call(&ctx, entry_id);
  irwriter_br(w, succ_bb);

  // Emit per-rule labeled blocks
  for (int32_t i = 0; i < n_rules; i++) {
    ScopedRule* rule = &sc->rules[i];

    irwriter_bb_at(w, irwriter_label_f(w, "%s$%s", sc->scope_name, rule->name));

    if (rule->kind == SCOPED_RULE_KIND_CALL) {
      irwriter_br(w, irwriter_label_f(w, "%s$%s", sc->scope_name, sc->rules[rule->as.call].name));
      continue;
    }

    if (!rule->needs_memo) {
      // Non-memoizable (TERM): compute and return via fast_ret
      IrLabel parse_fail_bb = irwriter_label(w);
      IrLabel ret_bb = irwriter_label(w);
      ctx.fail_label = parse_fail_bb;

      IrVal match_len;
      switch (rule->kind) {
      case SCOPED_RULE_KIND_TERM:
        match_len = peg_ir_term(&ctx, rule->as.term);
        break;
      default:
        match_len = irwriter_imm_int(w, 0);
        break;
      }

      irwriter_store(w, "i32", match_len, ret_a);
      irwriter_br(w, ret_bb);

      // fail path
      irwriter_bb_at(w, parse_fail_bb);
      irwriter_store(w, "i32", irwriter_imm_int(w, -1), ret_a);
      irwriter_br(w, ret_bb);

      // return via fast_ret
      irwriter_bb_at(w, ret_bb);
      IrVal ret_ptr = irwriter_load(w, "ptr", fast_ret_a);
      irwriter_rawf(w, "  indirectbr ptr ");
      irwriter_emit_val(w, ret_ptr);
      irwriter_rawf(w, ", []\n");

      ctx.fail_label = fail_bb;
      continue;
    }

    // Memoizable rule: memo check -> compute -> memo write -> return
    IrLabel rule_done_bb = irwriter_label(w);
    IrLabel rule_compute_bb = irwriter_label(w);
    IrLabel rule_miss_bb = irwriter_label(w);

    IrVal col = irwriter_load(w, "i64", col_a);

    if (compress && n_sg > 0) {
      // row_shared: bit test first
      IrVal bit_ok = peg_ir_bit_test(&ctx, col, rule->segment_index, rule->rule_bit_mask);
      IrLabel bit_pass_bb = irwriter_label(w);
      irwriter_br_cond(w, bit_ok, bit_pass_bb, rule_miss_bb);
      irwriter_bb_at(w, bit_pass_bb);
      // check slot
      IrVal cached = peg_ir_read_slot(&ctx, col, rule->slot_index);
      IrVal neg1 = irwriter_imm_int(w, -1);
      IrVal is_cached = irwriter_icmp(w, "ne", "i32", cached, neg1);
      irwriter_br_cond(w, is_cached, rule_done_bb, rule_compute_bb);
    } else {
      // naive: check slot directly
      IrVal cached = peg_ir_read_slot(&ctx, col, rule->slot_index);
      IrVal neg1 = irwriter_imm_int(w, -1);
      IrVal is_cached = irwriter_icmp(w, "ne", "i32", cached, neg1);
      irwriter_br_cond(w, is_cached, rule_done_bb, rule_compute_bb);
    }

    // compute: do the parse
    irwriter_bb_at(w, rule_compute_bb);
    IrLabel parse_fail_bb = irwriter_label(w);
    ctx.fail_label = parse_fail_bb;

    IrVal match_len;
    switch (rule->kind) {
    case SCOPED_RULE_KIND_TERM:
      match_len = peg_ir_term(&ctx, rule->as.term);
      break;
    case SCOPED_RULE_KIND_CALL:
      match_len = peg_ir_call(&ctx, rule->as.call);
      break;
    case SCOPED_RULE_KIND_SEQ:
      match_len = peg_ir_seq(&ctx, rule->as.seq);
      break;
    case SCOPED_RULE_KIND_BRANCHES:
      match_len = peg_ir_choice(&ctx, rule->as.branches);
      break;
    case SCOPED_RULE_KIND_JOIN:
      match_len = irwriter_imm_int(w, 0);
      break;
    }

    // write to memo table
    peg_ir_write_slot(&ctx, col, rule->slot_index, match_len);
    if (compress && n_sg > 0) {
      peg_ir_bit_exclude(&ctx, col, rule->segment_index, rule->rule_bit_mask);
    }
    irwriter_store(w, "i32", match_len, ret_a);
    irwriter_br(w, rule_done_bb);

    // parse fail
    irwriter_bb_at(w, parse_fail_bb);
    if (compress && n_sg > 0) {
      peg_ir_bit_deny(&ctx, col, rule->segment_index, rule->rule_bit_mask);
    }
    irwriter_br(w, rule_miss_bb);

    // miss: set ret to -1 for failure
    irwriter_bb_at(w, rule_miss_bb);
    irwriter_store(w, "i32", irwriter_imm_int(w, -1), ret_a);

    // return via stack
    IrVal sp = irwriter_load(w, "ptr", sp_a);
    IrVal ret_ptr = irwriter_load(w, "ptr", sp);
    IrVal new_sp = irwriter_next_reg(w);
    irwriter_rawf(w, "  %%r%d = getelementptr i64, ptr ", (int)new_sp);
    irwriter_emit_val(w, sp);
    irwriter_rawf(w, ", i64 -1\n");
    irwriter_store(w, "ptr", new_sp, sp_a);
    irwriter_rawf(w, "  indirectbr ptr ");
    irwriter_emit_val(w, ret_ptr);
    irwriter_rawf(w, ", []\n");

    // done: cached value already in slot, load into ret
    irwriter_bb_at(w, rule_done_bb);
    IrVal slot_val = peg_ir_read_slot(&ctx, col, rule->slot_index);
    irwriter_store(w, "i32", slot_val, ret_a);

    IrVal sp2 = irwriter_load(w, "ptr", sp_a);
    IrVal ret_ptr2 = irwriter_load(w, "ptr", sp2);
    IrVal new_sp2 = irwriter_next_reg(w);
    irwriter_rawf(w, "  %%r%d = getelementptr i64, ptr ", (int)new_sp2);
    irwriter_emit_val(w, sp2);
    irwriter_rawf(w, ", i64 -1\n");
    irwriter_store(w, "ptr", new_sp2, sp_a);
    irwriter_rawf(w, "  indirectbr ptr ");
    irwriter_emit_val(w, ret_ptr2);
    irwriter_rawf(w, ", []\n");

    ctx.fail_label = fail_bb; // restore for next rule
  }

  // fail: free table
  irwriter_bb_at(w, fail_bb);
  irwriter_rawf(w, "  call void @free(ptr ");
  irwriter_emit_val(w, tbl);
  irwriter_rawf(w, ")\n");
  irwriter_store(w, "ptr", irwriter_imm(w, "null"), val_pp);
  irwriter_ret_void(w);

  // success
  irwriter_bb_at(w, succ_bb);
  irwriter_ret_void(w);

  irwriter_define_end(w);
}

// ============================================================
// Header generation — real decode loaders
// ============================================================

static void _gen_header(PegGenInput* input, ScopeClosure* closures, int32_t n_closures, HeaderWriter* hw, bool compress,
                        const char* prefix) {
  (void)compress;
  hw_pragma_once(hw);
  hw_blank(hw);
  hw_include_sys(hw, "stdint.h");
  hw_include_sys(hw, "stdbool.h");
  hw_include_sys(hw, "string.h");
  hw_blank(hw);

  hw_raw(hw, "typedef struct {\n  void* tc;\n  int32_t col;\n  int32_t next_col;\n} PegRef;\n\n");

  hw_fmt(hw, "static inline bool %s_has_next(PegRef ref) {\n", prefix);
  hw_raw(hw, "  return ref.col < ref.next_col;\n}\n\n");

  hw_fmt(hw, "static inline PegRef %s_get_next(PegRef ref) {\n", prefix);
  hw_raw(hw, "  return (PegRef){ref.tc, ref.col + 1, ref.next_col};\n}\n\n");

  // Helper to read a slot from the memoize table
  hw_fmt(hw, "static inline int32_t %s_read_slot(PegRef ref, int32_t col_sizeof, int32_t slot_offset) {\n", prefix);
  hw_raw(hw, "  // tc->value is at offset 8 in TokenChunk\n");
  hw_raw(hw, "  void* table = *(void**)((char*)ref.tc + 8);\n");
  hw_raw(hw, "  int32_t* slot_ptr = (int32_t*)((char*)table + ref.col * col_sizeof + slot_offset);\n");
  hw_raw(hw, "  return *slot_ptr;\n");
  hw_fmt(hw, "}\n\n");

  // For each declared rule, generate node struct + loader
  for (int32_t ri = 0; ri < (int32_t)darray_size(input->rules); ri++) {
    PegRule* rule = &input->rules[ri];
    const char* rn = symtab_get(&input->rule_names, rule->global_id);
    PegUnit* body = &rule->body;

    bool has_branches = body->kind == PEG_BRANCHES;
    int32_t nc = 0;
    if (body->kind == PEG_SEQ) {
      nc = body->children ? (int32_t)darray_size(body->children) : 0;
      for (int32_t i = 0; i < nc; i++) {
        if (body->children[i].kind == PEG_BRANCHES) {
          has_branches = true;
        }
      }
    }

    int nn_len = snprintf(NULL, 0, "%s_%s_Node", prefix, rn);
    char* nn = malloc((size_t)nn_len + 1);
    snprintf(nn, (size_t)nn_len + 1, "%s_%s_Node", prefix, rn);

    hw_fmt(hw, "typedef struct {\n");

    if (has_branches) {
      hw_raw(hw, "  struct {\n");
      if (body->kind == PEG_BRANCHES) {
        for (int32_t j = 0; j < (int32_t)darray_size(body->children); j++) {
          const char* tag = body->children[j].tag;
          if (tag && tag[0]) {
            hw_fmt(hw, "    bool %s : 1;\n", tag);
          }
        }
      } else {
        for (int32_t i = 0; i < nc; i++) {
          if (body->children[i].kind != PEG_BRANCHES) {
            continue;
          }
          PegUnit* br = &body->children[i];
          for (int32_t j = 0; j < (int32_t)darray_size(br->children); j++) {
            const char* tag = br->children[j].tag;
            if (tag && tag[0]) {
              hw_fmt(hw, "    bool %s : 1;\n", tag);
            }
          }
        }
      }
      hw_raw(hw, "  } is;\n");
    }

    if (body->kind == PEG_TERM || body->kind == PEG_CALL) {
      const char* fn_name = NULL;
      if (body->tag && body->tag[0]) {
        fn_name = body->tag;
      } else if (body->kind == PEG_CALL) {
        fn_name = symtab_get(&input->rule_names, body->id);
      } else {
        fn_name = body->id >= input->tokens.start_num ? symtab_get(&input->tokens, body->id)
                                                      : symtab_get(&input->scope_names, body->id);
      }
      if (fn_name) {
        hw_fmt(hw, "  PegRef %s;\n", fn_name);
      }
    } else {
      for (int32_t i = 0; i < nc; i++) {
        PegUnit* child = &body->children[i];
        const char* fn_name = NULL;
        if (child->tag && child->tag[0]) {
          fn_name = child->tag;
        } else if (child->kind == PEG_CALL) {
          fn_name = symtab_get(&input->rule_names, child->id);
        } else if (child->kind == PEG_TERM) {
          fn_name = child->id >= input->tokens.start_num ? symtab_get(&input->tokens, child->id)
                                                         : symtab_get(&input->scope_names, child->id);
        }
        if (fn_name && child->kind != PEG_BRANCHES) {
          hw_fmt(hw, "  PegRef %s;\n", fn_name);
        }
      }
    }

    hw_fmt(hw, "} %s;\n\n", nn);

    // Find the scope closure and the scoped rule for this declared rule
    // so we can emit a real loader that decodes from the memoize table
    int32_t rule_slot = -1;
    int32_t scope_col_sizeof = 4;
    int32_t scope_slots_offset = 0;
    for (int32_t ci = 0; ci < n_closures; ci++) {
      ScopeClosure* sc = &closures[ci];
      int32_t sid = symtab_find(&sc->defined_rules, rn);
      if (sid >= 0 && sid < (int32_t)darray_size(sc->root_ids)) {
        int32_t root = sc->root_ids[sid];
        if (root >= 0 && root < (int32_t)darray_size(sc->rules)) {
          rule_slot = (int32_t)sc->rules[root].slot_index;

          // compute col_sizeof for this scope
          int32_t nsg = 0, nsl = 0;
          for (int32_t k = 0; k < (int32_t)darray_size(sc->rules); k++) {
            if ((int32_t)sc->rules[k].segment_index + 1 > nsg) {
              nsg = (int32_t)sc->rules[k].segment_index + 1;
            }
            if ((int32_t)sc->rules[k].slot_index + 1 > nsl) {
              nsl = (int32_t)sc->rules[k].slot_index + 1;
            }
          }
          scope_col_sizeof = nsg * 4 + nsl * 4;
          if (scope_col_sizeof == 0) {
            scope_col_sizeof = 4;
          }
          scope_slots_offset = nsg * 4;
        }
        break;
      }
    }

    // Generate loader that decodes from memoize table
    hw_fmt(hw, "static inline %s %s_load_%s(PegRef ref) {\n", nn, prefix, rn);
    hw_fmt(hw, "  %s node;\n", nn);
    hw_fmt(hw, "  memset(&node, 0, sizeof(node));\n");

    if (rule_slot >= 0) {
      int32_t slot_byte = scope_slots_offset + rule_slot * 4;
      hw_fmt(hw, "  int32_t val = %s_read_slot(ref, %d, %d);\n", prefix, scope_col_sizeof, slot_byte);

      if (has_branches) {
        int32_t branch_idx = 0;
        if (body->kind == PEG_BRANCHES) {
          for (int32_t j = 0; j < (int32_t)darray_size(body->children); j++) {
            const char* tag = body->children[j].tag;
            if (tag && tag[0]) {
              hw_fmt(hw, "  node.is.%s = (val == %d);\n", tag, branch_idx);
            }
            branch_idx++;
          }
        } else {
          for (int32_t i = 0; i < nc; i++) {
            if (body->children[i].kind != PEG_BRANCHES) {
              continue;
            }
            PegUnit* br = &body->children[i];
            for (int32_t j = 0; j < (int32_t)darray_size(br->children); j++) {
              const char* tag = br->children[j].tag;
              if (tag && tag[0]) {
                hw_fmt(hw, "  node.is.%s = (val == %d);\n", tag, branch_idx);
              }
              branch_idx++;
            }
          }
        }
      }
      hw_raw(hw, "  (void)val;\n");
    }

    hw_fmt(hw, "  return node;\n");
    hw_fmt(hw, "}\n\n");
    free(nn);
  }

  // scope size defines
  for (int32_t ci = 0; ci < n_closures; ci++) {
    ScopeClosure* sc = &closures[ci];
    int32_t nr = (int32_t)darray_size(sc->rules);
    int dn_len = snprintf(NULL, 0, "%s_%s_N_RULES", prefix, sc->scope_name);
    char* dn = malloc((size_t)dn_len + 1);
    snprintf(dn, (size_t)dn_len + 1, "%s_%s_N_RULES", prefix, sc->scope_name);
    for (char* p = dn; *p; p++) {
      if (*p >= 'a' && *p <= 'z') {
        *p = (char)(*p - 32);
      }
    }
    hw_define(hw, dn, nr);
    free(dn);
  }
  hw_blank(hw);
}

// ============================================================
// peg_gen
// ============================================================

void peg_gen(PegGenInput* input, HeaderWriter* hw, IrWriter* w, bool compress_memoize, const char* prefix) {
  int32_t n_closures = 0;
  ScopeClosure* closures = _gather_scope_closures(input, &n_closures);

  _breakdown_rules(input, closures, n_closures);

  for (int32_t ci = 0; ci < n_closures; ci++) {
    ScopeClosure* sc = &closures[ci];
    int32_t nr = (int32_t)darray_size(sc->rules);
    for (int32_t j = 0; j < nr; j++) {
      sc->rules[j].needs_memo =
          (sc->rules[j].kind != SCOPED_RULE_KIND_TERM && sc->rules[j].kind != SCOPED_RULE_KIND_CALL);
    }
    _sort_memoizables(sc);
    nr = (int32_t)darray_size(sc->rules);
    if (input->verbose) {
      int32_t n_defined = symtab_count(&sc->defined_rules);
      int32_t n_memo = 0;
      for (int32_t j = 0; j < nr; j++) {
        if (sc->rules[j].needs_memo) {
          n_memo++;
        }
      }
      fprintf(stderr, "  [peg] %s: %d defined, %d broken-down (%d memo)\n", sc->scope_name, n_defined, nr, n_memo);
    }
    if (input->verbose > 1) {
      for (int32_t j = 0; j < nr; j++) {
        ScopedRule* r = &sc->rules[j];
        const char* mul = r->multiplier == '?' ? "?" : r->multiplier == '*' ? "*" : r->multiplier == '+' ? "+" : "";
        switch (r->kind) {
        case SCOPED_RULE_KIND_TERM: {
          const char* tn = r->as.term >= input->tokens.start_num ? symtab_get(&input->tokens, r->as.term)
                                                                 : symtab_get(&input->scope_names, r->as.term);
          fprintf(stderr, "    [%d] %s = term(%s)%s\n", j, r->name, tn ? tn : "?", mul);
          break;
        }
        case SCOPED_RULE_KIND_CALL:
          fprintf(stderr, "    [%d] %s = call(%d:%s)%s\n", j, r->name, r->as.call,
                  r->as.call >= 0 && r->as.call < nr ? sc->rules[r->as.call].name : "?", mul);
          break;
        case SCOPED_RULE_KIND_SEQ: {
          fprintf(stderr, "    [%d] %s = seq(", j, r->name);
          for (int32_t k = 0; k < (int32_t)darray_size(r->as.seq); k++) {
            if (k) {
              fprintf(stderr, ", ");
            }
            fprintf(stderr, "%d", r->as.seq[k]);
          }
          fprintf(stderr, ")%s\n", mul);
          break;
        }
        case SCOPED_RULE_KIND_BRANCHES: {
          fprintf(stderr, "    [%d] %s = branches(", j, r->name);
          for (int32_t k = 0; k < (int32_t)darray_size(r->as.branches); k++) {
            if (k) {
              fprintf(stderr, ", ");
            }
            fprintf(stderr, "%d", r->as.branches[k]);
          }
          fprintf(stderr, ")%s\n", mul);
          break;
        }
        case SCOPED_RULE_KIND_JOIN:
          fprintf(stderr, "    [%d] %s = join(%d, %d)%s\n", j, r->name, r->as.join.lhs, r->as.join.rhs, mul);
          break;
        }
      }
    }
  }

  for (int32_t i = 0; i < n_closures; i++) {
    _compute_nullable(&closures[i]);
    _compute_first_last(&closures[i]);
    if (compress_memoize) {
      _assign_colored(&closures[i], input->verbose);
    } else {
      _assign_naive(&closures[i]);
    }
    if (input->verbose) {
      ScopeClosure* sc = &closures[i];
      int32_t n_slots = 0;
      for (int32_t j = 0; j < (int32_t)darray_size(sc->rules); j++) {
        if (sc->rules[j].needs_memo && (int32_t)sc->rules[j].slot_index + 1 > n_slots) {
          n_slots = (int32_t)sc->rules[j].slot_index + 1;
        }
      }
      fprintf(stderr, "  [peg] %s: %d slots after coloring\n", sc->scope_name, n_slots);
    }
  }

  peg_ir_emit_helpers(w);

  for (int32_t i = 0; i < n_closures; i++) {
    _gen_scope(input, &closures[i], w, compress_memoize);
  }

  _gen_header(input, closures, n_closures, hw, compress_memoize, prefix);

  // cleanup
  for (int32_t i = 0; i < n_closures; i++) {
    symtab_free(&closures[i].defined_rules);
    for (int32_t j = 0; j < (int32_t)darray_size(closures[i].rules); j++) {
      ScopedRule* r = &closures[i].rules[j];
      bitset_del(r->first_set);
      bitset_del(r->last_set);
      if (r->kind == SCOPED_RULE_KIND_SEQ) {
        darray_del(r->as.seq);
      } else if (r->kind == SCOPED_RULE_KIND_BRANCHES) {
        darray_del(r->as.branches);
      }
    }
    darray_del(closures[i].rules);
    darray_del(closures[i].root_ids);
  }
  darray_del(closures);
}
