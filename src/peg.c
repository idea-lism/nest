// specs/peg.md

#include "peg.h"
#include "bitset.h"
#include "coloring.h"
#include "darray.h"
#include "graph.h"
#include "peg_ir.h"
#include "symtab.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Helpers ---

static const char* _scope_name(PegRule* rule) { return (rule->scope && rule->scope[0]) ? rule->scope : "main"; }

static int32_t _is_scope_entry(PegRule* rule) { return rule->scope && rule->scope[0]; }

static int32_t _global_rule_index(PegRule* rules, int32_t n_rules, const char* name) {
  for (int32_t i = 0; i < n_rules; i++) {
    if (strcmp(rules[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

static int32_t _closure_index(ScopeClosure* closures, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(closures); i++) {
    if (strcmp(closures[i].scope_name, name) == 0) {
      return i;
    }
  }
  return -1;
}

// --- ScopedRule helpers ---

static int32_t _push_rule(ScopeClosure* cl, ScopedRule sr) {
  int32_t id = (int32_t)darray_size(cl->rules);
  darray_push(cl->rules, sr);
  return id;
}

static char* _synth_name(const char* owner, int32_t id) {
  int32_t len = snprintf(NULL, 0, "%s$%d", owner, id) + 1;
  char* buf = malloc((size_t)len);
  snprintf(buf, (size_t)len, "%s$%d", owner, id);
  return buf;
}

// --- Breakdown: PegUnit tree -> ScopedRule graph ---
//
// Appends ScopedRules to cl->rules. Returns the scoped_rule_id of the produced root.
// For CALL: uses defined_rules symtab to resolve. Unresolvable calls get as.call = -1.

static int32_t _breakdown(ScopeClosure* cl, PegUnit* unit, const char* owner_name, int32_t* next_synth,
                          PegRule* all_rules, int32_t n_rules, Symtab* tokens);

static int32_t _breakdown_leaf(ScopeClosure* cl, PegUnit* unit, const char* owner_name, int32_t* next_synth,
                               PegRule* all_rules, int32_t n_rules, Symtab* tokens) {
  // Interlace => JOIN
  if (unit->interlace && unit->ninterlace > 0) {
    PegUnit plain = *unit;
    plain.multiplier = 0;
    plain.interlace = NULL;
    plain.ninterlace = 0;
    int32_t lhs = _breakdown(cl, &plain, owner_name, next_synth, all_rules, n_rules, tokens);
    int32_t rhs = _breakdown(cl, unit->interlace, owner_name, next_synth, all_rules, n_rules, tokens);
    char* name = _synth_name(owner_name, (*next_synth)++);
    ScopedRule sr = {
        .name = name,
        .kind = SCOPED_RULE_KIND_JOIN,
        .as.join = {lhs, rhs},
        .multiplier = (char)unit->multiplier,
    };
    return _push_rule(cl, sr);
  }

  if (unit->kind == PEG_TOK || unit->kind == PEG_KEYWORD_TOK) {
    ScopedRule sr = {
        .name = strdup(unit->name),
        .kind = SCOPED_RULE_KIND_TOK,
        .as.tok = symtab_intern(tokens, unit->name),
        .multiplier = (char)unit->multiplier,
    };
    return _push_rule(cl, sr);
  }

  // PEG_ID
  int32_t gi = _global_rule_index(all_rules, n_rules, unit->name);
  if (gi >= 0 && _is_scope_entry(&all_rules[gi])) {
    ScopedRule sr = {
        .name = strdup(unit->name),
        .kind = SCOPED_RULE_KIND_EXTERNAL_SCOPE,
        .as.external_scope = gi,
        .multiplier = (char)unit->multiplier,
    };
    return _push_rule(cl, sr);
  }

  // Regular call — resolve via defined_rules
  int32_t target = symtab_find(&cl->defined_rules, unit->name);
  ScopedRule sr = {
      .name = strdup(unit->name),
      .kind = SCOPED_RULE_KIND_CALL,
      .as.call = target, // resolved if rule was pre-registered, -1 otherwise
      .multiplier = (char)unit->multiplier,
  };
  return _push_rule(cl, sr);
}

static int32_t _breakdown(ScopeClosure* cl, PegUnit* unit, const char* owner_name, int32_t* next_synth,
                          PegRule* all_rules, int32_t n_rules, Symtab* tokens) {
  if (unit->kind == PEG_TOK || unit->kind == PEG_KEYWORD_TOK || unit->kind == PEG_ID) {
    return _breakdown_leaf(cl, unit, owner_name, next_synth, all_rules, n_rules, tokens);
  }

  if (unit->kind == PEG_SEQ) {
    int32_t n = unit->children ? (int32_t)darray_size(unit->children) : 0;
    int32_t* child_ids = darray_new(sizeof(int32_t), 0);
    for (int32_t i = 0; i < n; i++) {
      PegUnit* child = &unit->children[i];
      int32_t child_id;
      if (child->kind == PEG_BRANCHES) {
        char* sname = _synth_name(owner_name, (*next_synth)++);
        child_id = _breakdown(cl, child, sname, next_synth, all_rules, n_rules, tokens);
        cl->rules[child_id].name = sname;
      } else {
        child_id = _breakdown(cl, child, owner_name, next_synth, all_rules, n_rules, tokens);
      }
      darray_push(child_ids, child_id);
    }
    if ((int32_t)darray_size(child_ids) == 1) {
      int32_t only = child_ids[0];
      darray_del(child_ids);
      return only;
    }
    ScopedRule sr = {.name = strdup(owner_name), .kind = SCOPED_RULE_KIND_SEQ, .as.seq = child_ids};
    return _push_rule(cl, sr);
  }

  if (unit->kind == PEG_BRANCHES) {
    int32_t n = unit->children ? (int32_t)darray_size(unit->children) : 0;
    int32_t* branch_ids = darray_new(sizeof(int32_t), 0);
    for (int32_t i = 0; i < n; i++) {
      int32_t alt_id = _breakdown(cl, &unit->children[i], owner_name, next_synth, all_rules, n_rules, tokens);
      darray_push(branch_ids, alt_id);
    }
    ScopedRule sr = {.name = strdup(owner_name), .kind = SCOPED_RULE_KIND_BRANCHES, .as.branches = branch_ids};
    return _push_rule(cl, sr);
  }

  // Fallback: empty seq
  int32_t* empty = darray_new(sizeof(int32_t), 0);
  ScopedRule sr = {.name = strdup(owner_name), .kind = SCOPED_RULE_KIND_SEQ, .as.seq = empty};
  return _push_rule(cl, sr);
}

// Post-pass: resolve CALL references and branch-wrap
static void _resolve_and_wrap(ScopeClosure* cl) {
  // Resolve unresolved CALLs
  for (int32_t i = 0; i < (int32_t)darray_size(cl->rules); i++) {
    ScopedRule* sr = &cl->rules[i];
    if (sr->kind == SCOPED_RULE_KIND_CALL && sr->as.call == -1 && sr->name) {
      sr->as.call = symtab_find(&cl->defined_rules, sr->name);
    }
  }

  // Branch-wrapping: if a BRANCHES child directly CALLs another BRANCHES rule, wrap in SEQ
  for (int32_t i = 0; i < (int32_t)darray_size(cl->rules); i++) {
    if (cl->rules[i].kind != SCOPED_RULE_KIND_BRANCHES) {
      continue;
    }
    int32_t nb = (int32_t)darray_size(cl->rules[i].as.branches);
    for (int32_t j = 0; j < nb; j++) {
      int32_t alt_id = cl->rules[i].as.branches[j];
      if (alt_id < 0 || alt_id >= (int32_t)darray_size(cl->rules)) {
        continue;
      }
      if (cl->rules[alt_id].kind != SCOPED_RULE_KIND_CALL) {
        continue;
      }
      int32_t target = cl->rules[alt_id].as.call;
      if (target < 0 || target >= (int32_t)darray_size(cl->rules)) {
        continue;
      }
      if (cl->rules[target].kind != SCOPED_RULE_KIND_BRANCHES) {
        continue;
      }
      // Wrap
      int32_t* wrap_seq = darray_new(sizeof(int32_t), 0);
      darray_push(wrap_seq, alt_id);
      ScopedRule wrapper = {.name = strdup(cl->rules[alt_id].name), .kind = SCOPED_RULE_KIND_SEQ, .as.seq = wrap_seq};
      int32_t wrapper_id = _push_rule(cl, wrapper);
      // Refresh pointer (darray may realloc)
      cl->rules[i].as.branches[j] = wrapper_id;
    }
  }
}

// --- Scope closure gathering + breakdown ---

static void _walk_closure_units(PegUnit* unit, PegRule* rules, int32_t n_rules, Bitset* visited,
                                int32_t** out_indices) {
  if (unit->kind == PEG_ID) {
    int32_t ri = _global_rule_index(rules, n_rules, unit->name);
    if (ri >= 0 && !bitset_contains(visited, (uint32_t)ri)) {
      if (_is_scope_entry(&rules[ri])) {
        return;
      }
      bitset_add_bit(visited, (uint32_t)ri);
      darray_push(*out_indices, ri);
      _walk_closure_units(&rules[ri].seq, rules, n_rules, visited, out_indices);
    }
    return;
  }
  if (unit->children) {
    for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
      _walk_closure_units(&unit->children[i], rules, n_rules, visited, out_indices);
    }
  }
  if (unit->interlace) {
    _walk_closure_units(unit->interlace, rules, n_rules, visited, out_indices);
  }
}

// Build scope closures, break down rules, resolve calls.
// Returns darray of ScopeClosure.
// Also populates root_ids[closure_idx] = darray of (global_rule_idx, root_scoped_rule_id) pairs.
static ScopeClosure* _build_closures(PegRule* rules, int32_t n_rules, Symtab* tokens) {
  ScopeClosure* closures = darray_new(sizeof(ScopeClosure), 0);
  int32_t** all_seeds = darray_new(sizeof(int32_t*), 0);

  for (int32_t i = 0; i < n_rules; i++) {
    const char* sname = _scope_name(&rules[i]);
    int32_t si = _closure_index(closures, sname);
    if (si < 0) {
      ScopeClosure cl = {0};
      cl.scope_name = strdup(sname);
      symtab_init(&cl.defined_rules, 0);
      cl.rules = darray_new(sizeof(ScopedRule), 0);
      darray_push(closures, cl);
      int32_t* s = darray_new(sizeof(int32_t), 0);
      darray_push(all_seeds, s);
      si = (int32_t)darray_size(closures) - 1;
    }
    darray_push(all_seeds[si], i);
  }

  for (int32_t si = 0; si < (int32_t)darray_size(closures); si++) {
    // Expand closure
    Bitset* visited = bitset_new();
    int32_t seed_count = (int32_t)darray_size(all_seeds[si]);
    for (int32_t j = 0; j < seed_count; j++) {
      bitset_add_bit(visited, (uint32_t)all_seeds[si][j]);
    }
    for (int32_t j = 0; j < seed_count; j++) {
      _walk_closure_units(&rules[all_seeds[si][j]].seq, rules, n_rules, visited, &all_seeds[si]);
    }
    bitset_del(visited);

    // Reserve slots for user-visible rules. The symtab will assign IDs 0, 1, 2, ...
    // We push placeholder ScopedRules at those positions.
    int32_t n_scope = (int32_t)darray_size(all_seeds[si]);
    for (int32_t j = 0; j < n_scope; j++) {
      int32_t gi = all_seeds[si][j];
      ScopedRule placeholder = {.name = strdup(rules[gi].name), .kind = SCOPED_RULE_KIND_SEQ};
      placeholder.as.seq = NULL; // will be filled in
      _push_rule(&closures[si], placeholder);
      symtab_intern(&closures[si].defined_rules, rules[gi].name);
      // Now symtab_find(name) == j, and rules[j] is the placeholder
    }

    // Break down each rule body and fill in the reserved slot
    for (int32_t j = 0; j < n_scope; j++) {
      int32_t gi = all_seeds[si][j];
      int32_t next_synth = 1;

      // Breakdown the rule body. This appends intermediates to cl->rules after the reserved slots.
      int32_t body_id = _breakdown(&closures[si], &rules[gi].seq, rules[gi].name, &next_synth, rules, n_rules, tokens);

      // Copy the broken-down body into the reserved slot
      ScopedRule body = closures[si].rules[body_id];
      free((char*)closures[si].rules[j].name);
      closures[si].rules[j] = body;
      free((char*)closures[si].rules[j].name);
      closures[si].rules[j].name = strdup(rules[gi].name);

      // Null out the original to prevent double-free
      if (body_id != j) {
        memset(&closures[si].rules[body_id], 0, sizeof(ScopedRule));
      }
    }

    _resolve_and_wrap(&closures[si]);
  }

  for (int32_t i = 0; i < (int32_t)darray_size(all_seeds); i++) {
    darray_del(all_seeds[i]);
  }
  darray_del(all_seeds);

  return closures;
}

static void _free_closures(ScopeClosure* closures) {
  for (int32_t i = 0; i < (int32_t)darray_size(closures); i++) {
    ScopeClosure* cl = &closures[i];
    for (int32_t j = 0; j < (int32_t)darray_size(cl->rules); j++) {
      ScopedRule* sr = &cl->rules[j];
      if (sr->kind == SCOPED_RULE_KIND_BRANCHES && sr->as.branches) {
        darray_del(sr->as.branches);
      } else if (sr->kind == SCOPED_RULE_KIND_SEQ && sr->as.seq) {
        darray_del(sr->as.seq);
      }
      if (sr->first_set) {
        bitset_del(sr->first_set);
      }
      if (sr->last_set) {
        bitset_del(sr->last_set);
      }
      free((char*)sr->name);
    }
    darray_del(cl->rules);
    symtab_free(&cl->defined_rules);
    free((char*)cl->scope_name);
  }
  darray_del(closures);
}

// --- Nullable / first-set / last-set analysis ---

static void _analyze_rule(ScopeClosure* cl, int32_t rule_id, Bitset* visiting, Symtab* symbols);

static void _ensure_analyzed(ScopeClosure* cl, int32_t rule_id, Bitset* visiting, Symtab* symbols) {
  if (rule_id < 0 || rule_id >= (int32_t)darray_size(cl->rules)) {
    return;
  }
  if (!cl->rules[rule_id].first_set) {
    _analyze_rule(cl, rule_id, visiting, symbols);
  }
}

static void _analyze_rule(ScopeClosure* cl, int32_t rule_id, Bitset* visiting, Symtab* symbols) {
  if (rule_id < 0 || rule_id >= (int32_t)darray_size(cl->rules)) {
    return;
  }
  ScopedRule* sr = &cl->rules[rule_id];
  if (sr->first_set) {
    return;
  }
  if (bitset_contains(visiting, (uint32_t)rule_id)) {
    sr->first_set = bitset_new();
    sr->last_set = bitset_new();
    sr->nullable = false;
    return;
  }
  bitset_add_bit(visiting, (uint32_t)rule_id);

  sr->first_set = bitset_new();
  sr->last_set = bitset_new();
  sr->nullable = false;

  switch (sr->kind) {
  case SCOPED_RULE_KIND_TOK:
    bitset_add_bit(sr->first_set, (uint32_t)sr->as.tok);
    bitset_add_bit(sr->last_set, (uint32_t)sr->as.tok);
    sr->nullable = false;
    break;

  case SCOPED_RULE_KIND_CALL:
    if (sr->as.call >= 0) {
      _ensure_analyzed(cl, sr->as.call, visiting, symbols);
      ScopedRule* target = &cl->rules[sr->as.call];
      Bitset* f = bitset_or(sr->first_set, target->first_set);
      bitset_del(sr->first_set);
      sr->first_set = f;
      Bitset* l = bitset_or(sr->last_set, target->last_set);
      bitset_del(sr->last_set);
      sr->last_set = l;
      sr->nullable = target->nullable;
    }
    break;

  case SCOPED_RULE_KIND_EXTERNAL_SCOPE: {
    char key[256];
    snprintf(key, sizeof(key), "scope:%s", sr->name);
    uint32_t sym = (uint32_t)symtab_intern(symbols, key);
    bitset_add_bit(sr->first_set, sym);
    bitset_add_bit(sr->last_set, sym);
    sr->nullable = false;
    break;
  }

  case SCOPED_RULE_KIND_SEQ: {
    int32_t n = sr->as.seq ? (int32_t)darray_size(sr->as.seq) : 0;
    if (n == 0) {
      sr->nullable = true;
      break;
    }
    sr->nullable = true;
    for (int32_t i = 0; i < n; i++) {
      _ensure_analyzed(cl, sr->as.seq[i], visiting, symbols);
      if (!cl->rules[sr->as.seq[i]].nullable) {
        sr->nullable = false;
        break;
      }
    }
    for (int32_t i = 0; i < n; i++) {
      _ensure_analyzed(cl, sr->as.seq[i], visiting, symbols);
      ScopedRule* child = &cl->rules[sr->as.seq[i]];
      Bitset* f = bitset_or(sr->first_set, child->first_set);
      bitset_del(sr->first_set);
      sr->first_set = f;
      if (!child->nullable) {
        break;
      }
    }
    for (int32_t i = n - 1; i >= 0; i--) {
      _ensure_analyzed(cl, sr->as.seq[i], visiting, symbols);
      ScopedRule* child = &cl->rules[sr->as.seq[i]];
      Bitset* l = bitset_or(sr->last_set, child->last_set);
      bitset_del(sr->last_set);
      sr->last_set = l;
      if (!child->nullable) {
        break;
      }
    }
    break;
  }

  case SCOPED_RULE_KIND_BRANCHES: {
    int32_t n = sr->as.branches ? (int32_t)darray_size(sr->as.branches) : 0;
    sr->nullable = false;
    for (int32_t i = 0; i < n; i++) {
      _ensure_analyzed(cl, sr->as.branches[i], visiting, symbols);
      ScopedRule* alt = &cl->rules[sr->as.branches[i]];
      Bitset* f = bitset_or(sr->first_set, alt->first_set);
      bitset_del(sr->first_set);
      sr->first_set = f;
      Bitset* l = bitset_or(sr->last_set, alt->last_set);
      bitset_del(sr->last_set);
      sr->last_set = l;
      if (alt->nullable) {
        sr->nullable = true;
      }
    }
    break;
  }

  case SCOPED_RULE_KIND_JOIN:
    _ensure_analyzed(cl, sr->as.join.lhs, visiting, symbols);
    {
      ScopedRule* lhs = &cl->rules[sr->as.join.lhs];
      Bitset* f = bitset_or(sr->first_set, lhs->first_set);
      bitset_del(sr->first_set);
      sr->first_set = f;
      Bitset* l = bitset_or(sr->last_set, lhs->last_set);
      bitset_del(sr->last_set);
      sr->last_set = l;
      sr->nullable = lhs->nullable;
    }
    break;
  }

  if (sr->multiplier == '?' || sr->multiplier == '*') {
    sr->nullable = true;
  }

  bitset_clear_bit(visiting, (uint32_t)rule_id);
}

static void _analyze_closure(ScopeClosure* cl, Symtab* symbols) {
  Bitset* visiting = bitset_new();
  for (int32_t i = 0; i < (int32_t)darray_size(cl->rules); i++) {
    _analyze_rule(cl, i, visiting, symbols);
  }
  bitset_del(visiting);
}

// --- Interference graph ---

static int32_t _are_exclusive(ScopedRule* a, ScopedRule* b) {
  if (a->nullable || b->nullable) {
    return 0;
  }
  Bitset* first_inter = bitset_and(a->first_set, b->first_set);
  int32_t first_empty = bitset_size(first_inter) == 0;
  bitset_del(first_inter);
  if (first_empty) {
    return 1;
  }
  Bitset* last_inter = bitset_and(a->last_set, b->last_set);
  int32_t last_empty = bitset_size(last_inter) == 0;
  bitset_del(last_inter);
  return last_empty;
}

static Graph* _build_interference_graph(ScopedRule* rules, int32_t n_rules) {
  Graph* g = graph_new(n_rules);
  for (int32_t i = 0; i < n_rules; i++) {
    for (int32_t j = i + 1; j < n_rules; j++) {
      if (!_are_exclusive(&rules[i], &rules[j])) {
        graph_add_edge(g, i, j);
      }
    }
  }
  return g;
}

// --- Header generation helpers ---

static void _make_struct_name(const char* rule_name, char* out, int32_t out_size) {
  snprintf(out, (size_t)out_size, "%sNode", rule_name);
  out[0] = (char)toupper((unsigned char)out[0]);
}

// Collect user-visible branch info from PegUnit tree (for header node type generation)
// Returns darray of PegUnit* pointing to branch alternatives
static PegUnit** _collect_branches(PegRule* rule) {
  PegUnit** all = darray_new(sizeof(PegUnit*), 0);
  if (!rule->seq.children) {
    return all;
  }
  for (int32_t i = 0; i < (int32_t)darray_size(rule->seq.children); i++) {
    if (rule->seq.children[i].kind == PEG_BRANCHES) {
      PegUnit* bu = &rule->seq.children[i];
      for (int32_t j = 0; j < (int32_t)darray_size(bu->children); j++) {
        darray_push(all, &bu->children[j]);
      }
    }
  }
  return all;
}

// Is scoped_rule_id a user-visible rule with branches?
static int32_t _has_branches(ScopeClosure* cl, int32_t rule_id) {
  ScopedRule* sr = &cl->rules[rule_id];
  if (sr->kind == SCOPED_RULE_KIND_BRANCHES) {
    return 1;
  }
  if (sr->kind == SCOPED_RULE_KIND_SEQ && sr->as.seq) {
    int32_t n = (int32_t)darray_size(sr->as.seq);
    for (int32_t i = 0; i < n; i++) {
      if (cl->rules[sr->as.seq[i]].kind == SCOPED_RULE_KIND_BRANCHES) {
        return 1;
      }
    }
  }
  return 0;
}

// --- Header: PegRef type ---

static void _gen_ref_type(HeaderWriter* hw) {
  hw_blank(hw);
  hw_raw(hw, "#include <stdint.h>\n");
  hw_raw(hw, "#include <stdbool.h>\n");
  hw_blank(hw);
  hw_struct_begin(hw, "PegRef");
  hw_field(hw, "void*", "table");
  hw_field(hw, "int32_t", "col");
  hw_field(hw, "int32_t", "next_col");
  hw_struct_end(hw);
  hw_raw(hw, " PegRef;\n\n");
}

// --- Header: node type for a user-visible rule ---

static void _gen_node_type(HeaderWriter* hw, PegRule* rule) {
  char struct_name[128];
  _make_struct_name(rule->name, struct_name, sizeof(struct_name));

  hw_struct_begin(hw, struct_name);

  PegUnit** branches = _collect_branches(rule);
  int32_t nbranches = (int32_t)darray_size(branches);

  if (nbranches > 0) {
    hw_raw(hw, "  struct {\n");
    for (int32_t i = 0; i < nbranches; i++) {
      PegUnit* b = branches[i];
      if (b->tag && b->tag[0]) {
        hw_fmt(hw, "    bool %s : 1;\n", b->tag);
      } else {
        hw_fmt(hw, "    bool branch%d : 1;\n", i);
      }
    }
    hw_raw(hw, "  } is;\n");
    for (int32_t i = 0; i < nbranches; i++) {
      PegUnit* b = branches[i];
      for (int32_t j = 0; j < (int32_t)darray_size(b->children); j++) {
        PegUnit* child = &b->children[j];
        if (child->name && child->name[0]) {
          hw_field(hw, "PegRef", child->name);
        }
      }
    }
  } else {
    if (rule->seq.children) {
      for (int32_t i = 0; i < (int32_t)darray_size(rule->seq.children); i++) {
        PegUnit* child = &rule->seq.children[i];
        if (child->name && child->name[0]) {
          hw_field(hw, "PegRef", child->name);
        }
      }
    }
  }

  darray_del(branches);
  hw_struct_end(hw);
  hw_fmt(hw, " %s;\n\n", struct_name);
}

// --- Header: Col type ---

static void _gen_col_type_naive(HeaderWriter* hw, ScopeClosure* cl) {
  char name[128];
  snprintf(name, sizeof(name), "Col_%s", cl->scope_name);
  int32_t n = cl->n_slots > 0 ? cl->n_slots : 1;
  hw_struct_begin(hw, name);
  hw_fmt(hw, "  int32_t slots[%d];\n", n);
  hw_struct_end(hw);
  hw_fmt(hw, " %s;\n\n", name);
}

static void _gen_col_type_shared(HeaderWriter* hw, ScopeClosure* cl) {
  char name[128];
  snprintf(name, sizeof(name), "Col_%s", cl->scope_name);
  int32_t n = cl->n_slots > 0 ? cl->n_slots : 1;
  hw_struct_begin(hw, name);
  hw_fmt(hw, "  int32_t bits[%d];\n", cl->n_bits);
  hw_fmt(hw, "  int32_t slots[%d];\n", n);
  hw_struct_end(hw);
  hw_fmt(hw, " %s;\n\n", name);
}

// --- LLVM IR Col type ---

static void _define_col_type_ir(IrWriter* w, ScopeClosure* cl) {
  int32_t n = cl->n_slots > 0 ? cl->n_slots : 1;
  if (cl->n_bits > 0) {
    irwriter_rawf(w, "%%%s = type { [%d x i32], [%d x i32] }\n", cl->col_type, cl->n_bits, n);
  } else {
    irwriter_rawf(w, "%%%s = type { [%d x i32] }\n", cl->col_type, n);
  }
}

// --- Header: load functions ---

// Build branch alt info from ScopedRule graph
typedef struct {
  int32_t slot_val;   // scoped_rule_id for rule-ref, -(idx+1) for non-rule
  int32_t child_slot; // slot_idx of the child rule, -1 for non-rule
  int32_t is_rule_ref;
} BranchAltInfo;

static BranchAltInfo* _build_branch_alts(ScopeClosure* cl, int32_t rule_id, int32_t* out_count) {
  ScopedRule* sr = &cl->rules[rule_id];

  // Find BRANCHES children
  int32_t* branch_rule_ids = NULL; // darray of scoped_rule_ids pointing to BRANCHES
  if (sr->kind == SCOPED_RULE_KIND_BRANCHES) {
    branch_rule_ids = darray_new(sizeof(int32_t), 0);
    darray_push(branch_rule_ids, rule_id);
  } else if (sr->kind == SCOPED_RULE_KIND_SEQ && sr->as.seq) {
    branch_rule_ids = darray_new(sizeof(int32_t), 0);
    for (int32_t i = 0; i < (int32_t)darray_size(sr->as.seq); i++) {
      int32_t cid = sr->as.seq[i];
      if (cl->rules[cid].kind == SCOPED_RULE_KIND_BRANCHES) {
        darray_push(branch_rule_ids, cid);
      }
    }
  }

  if (!branch_rule_ids || (int32_t)darray_size(branch_rule_ids) == 0) {
    if (branch_rule_ids) {
      darray_del(branch_rule_ids);
    }
    *out_count = 0;
    return NULL;
  }

  // Count total alts
  int32_t total = 0;
  for (int32_t i = 0; i < (int32_t)darray_size(branch_rule_ids); i++) {
    total += (int32_t)darray_size(cl->rules[branch_rule_ids[i]].as.branches);
  }

  BranchAltInfo* alts = calloc((size_t)total, sizeof(BranchAltInfo));
  int32_t idx = 0;
  for (int32_t i = 0; i < (int32_t)darray_size(branch_rule_ids); i++) {
    ScopedRule* br = &cl->rules[branch_rule_ids[i]];
    for (int32_t j = 0; j < (int32_t)darray_size(br->as.branches); j++) {
      int32_t alt_id = br->as.branches[j];
      ScopedRule* alt = &cl->rules[alt_id];

      // Check if this alt is a CALL (or a SEQ wrapping a CALL) to a user-visible rule
      int32_t target = -1;
      if (alt->kind == SCOPED_RULE_KIND_CALL && alt->as.call >= 0) {
        target = alt->as.call;
      } else if (alt->kind == SCOPED_RULE_KIND_SEQ && alt->as.seq && (int32_t)darray_size(alt->as.seq) == 1) {
        int32_t inner = alt->as.seq[0];
        if (cl->rules[inner].kind == SCOPED_RULE_KIND_CALL) {
          target = cl->rules[inner].as.call;
        }
      }

      if (target >= 0 && target < (int32_t)darray_size(cl->rules)) {
        alts[idx].slot_val = target;
        alts[idx].child_slot = (int32_t)cl->rules[target].slot_index;
        alts[idx].is_rule_ref = 1;
      } else {
        alts[idx].slot_val = -(idx + 1);
        alts[idx].child_slot = -1;
        alts[idx].is_rule_ref = 0;
      }
      idx++;
    }
  }

  darray_del(branch_rule_ids);
  *out_count = total;
  return alts;
}

static void _emit_child_load(HeaderWriter* hw, PegUnit* child, const char* cur_var, int32_t indent, ScopeClosure* cl) {
  const char* sp = indent >= 2 ? "    " : "  ";
  const char* inner = indent >= 2 ? "      " : "    ";
  const char* var = (child->name && child->name[0]) ? child->name : NULL;

  if (var) {
    hw_fmt(hw, "%snode.%s = (PegRef){table, %s, -1};\n", sp, var, cur_var);
  }

  if (child->kind == PEG_ID) {
    // Look up the child rule in this closure
    int32_t rid = symtab_find(&cl->defined_rules, child->name);
    int32_t slot = (rid >= 0) ? (int32_t)cl->rules[rid].slot_index : 0;
    int32_t child_has_branches = (rid >= 0) ? _has_branches(cl, rid) : 0;

    if (child_has_branches) {
      hw_fmt(hw, "%s{ int32_t sv = table[%s].slots[%d];\n", sp, cur_var, slot);
      hw_fmt(hw, "%sint32_t l = 0;\n", inner);
      hw_fmt(hw, "%sif (sv >= 0) {\n", inner);

      int32_t n_alts = 0;
      BranchAltInfo* alts = _build_branch_alts(cl, rid, &n_alts);
      int32_t first = 1;
      for (int32_t a = 0; a < n_alts; a++) {
        if (!alts[a].is_rule_ref) {
          continue;
        }
        hw_fmt(hw, "%s  %sif (sv == %d) l = table[%s].slots[%d];\n", inner, first ? "" : "else ", alts[a].slot_val,
               cur_var, alts[a].child_slot);
        first = 0;
      }
      free(alts);

      hw_fmt(hw, "%s} else if (sv <= -3) {\n", inner);
      hw_fmt(hw, "%s  l = (-(sv + 3)) & 0xFFFF;\n", inner);
      hw_fmt(hw, "%s}\n", inner);
      hw_fmt(hw, "%sif (l < 0) l = 0;\n", inner);
    } else {
      hw_fmt(hw, "%s{ int32_t l = table[%s].slots[%d];\n", sp, cur_var, slot);
      hw_fmt(hw, "%sif (l < 0) l = 0;\n", inner);
    }

    if (var && (child->multiplier == '+' || child->multiplier == '*')) {
      hw_fmt(hw, "%snode.%s.next_col = %s + l;\n", inner, var, cur_var);
    }
    hw_fmt(hw, "%s%s += l; }\n", inner, cur_var);
  } else if (child->kind == PEG_TOK) {
    hw_fmt(hw, "%s%s += 1;\n", sp, cur_var);
  }
}

static void _gen_load_impl(HeaderWriter* hw, PegRule* rule, int32_t rule_id, ScopeClosure* cl, const char* prefix) {
  char struct_name[128];
  _make_struct_name(rule->name, struct_name, sizeof(struct_name));

  hw_blank(hw);
  hw_fmt(hw, "static inline %s %s_load_%s(PegRef ref) {\n", struct_name, prefix, rule->name);
  hw_fmt(hw, "  %s node = {0};\n", struct_name);
  hw_fmt(hw, "  %s* table = (%s*)ref.table;\n", cl->hdr_col_type, cl->hdr_col_type);
  hw_fmt(hw, "  int32_t col = ref.col;\n");
  hw_fmt(hw, "  int32_t cur = col;\n");

  PegUnit** branches = _collect_branches(rule);
  int32_t nbranches = (int32_t)darray_size(branches);

  if (nbranches > 0) {
    hw_fmt(hw, "  int32_t branch_id = table[col].slots[%d];\n", (int32_t)cl->rules[rule_id].slot_index);

    int32_t n_alts = 0;
    BranchAltInfo* alts = _build_branch_alts(cl, rule_id, &n_alts);
    int32_t has_non_rule = 0;
    for (int32_t a = 0; a < n_alts; a++) {
      if (!alts[a].is_rule_ref) {
        has_non_rule = 1;
        break;
      }
    }
    if (has_non_rule) {
      hw_raw(hw, "  int32_t _pidx = (branch_id <= -3) ? ((-(branch_id + 3)) >> 16) : -1;\n");
    }

    int32_t branch_idx = 0;
    for (int32_t i = 0; i < (int32_t)darray_size(rule->seq.children); i++) {
      PegUnit* child = &rule->seq.children[i];
      if (child->kind == PEG_BRANCHES) {
        int32_t bn = (int32_t)darray_size(child->children);
        for (int32_t b = 0; b < bn; b++) {
          int32_t flat_idx = branch_idx + b;
          PegUnit* branch = &child->children[b];
          const char* tag = (branch->tag && branch->tag[0]) ? branch->tag : NULL;
          const char* field = tag;
          char field_buf[32];
          if (!field) {
            snprintf(field_buf, sizeof(field_buf), "branch%d", flat_idx);
            field = field_buf;
          }

          if (flat_idx < n_alts && alts[flat_idx].is_rule_ref) {
            hw_fmt(hw, "  node.is.%s = (branch_id == %d);\n", field, alts[flat_idx].slot_val);
            hw_fmt(hw, "  if (branch_id == %d) {\n", alts[flat_idx].slot_val);
          } else {
            hw_fmt(hw, "  node.is.%s = (_pidx == %d);\n", field, flat_idx);
            hw_fmt(hw, "  if (_pidx == %d) {\n", flat_idx);
          }

          hw_fmt(hw, "    int32_t bcur = cur;\n");
          for (int32_t j = 0; j < (int32_t)darray_size(branch->children); j++) {
            _emit_child_load(hw, &branch->children[j], "bcur", 2, cl);
          }
          hw_fmt(hw, "  }\n");
        }
        branch_idx += bn;
      } else {
        _emit_child_load(hw, child, "cur", 1, cl);
      }
    }
    free(alts);
  } else {
    if (rule->seq.children) {
      for (int32_t i = 0; i < (int32_t)darray_size(rule->seq.children); i++) {
        _emit_child_load(hw, &rule->seq.children[i], "cur", 1, cl);
      }
    }
  }

  darray_del(branches);
  hw_raw(hw, "  return node;\n");
  hw_raw(hw, "}\n");
}

// --- Rule function generation ---

static void _gen_rule_prologue(const char* rule_name, ScopeClosure* cl, IrWriter* w, char* col_type_ref,
                               int32_t col_type_ref_size) {
  const char* args[] = {"i8*", "i32", "i8*"};
  const char* arg_names[] = {"table", "col", "bt_stack"};
  char func_name[128];
  snprintf(func_name, sizeof(func_name), "parse_%s", rule_name);
  irwriter_define_start(w, func_name, "i32", 3, args, arg_names);
  irwriter_bb(w);
  snprintf(col_type_ref, (size_t)col_type_ref_size, "%%%s", cl->col_type);
}

static void _gen_rule_naive(int32_t rule_id, ScopeClosure* cl, IrWriter* w) {
  ScopedRule* sr = &cl->rules[rule_id];
  char col_type_ref[72];
  _gen_rule_prologue(sr->name, cl, w, col_type_ref, sizeof(col_type_ref));

  IrVal slot_val_ptr = irwriter_alloca(w, "i32");
  int32_t has_br = _has_branches(cl, rule_id);
  IrVal len_ptr = has_br ? irwriter_alloca(w, "i32") : -1;

  int32_t compute_bb = irwriter_label(w);
  int32_t fail_bb = irwriter_label(w);

  IrVal slot_reg = peg_ir_memo_get(w, col_type_ref, "%table", "%col", 0, (int32_t)cl->rules[rule_id].slot_index);

  int32_t not_uncached_bb = irwriter_label(w);
  irwriter_br_cond(w, irwriter_icmp(w, "ne", "i32", slot_reg, irwriter_imm(w, "-1")), not_uncached_bb, compute_bb);

  irwriter_bb_at(w, not_uncached_bb);
  int32_t cached_fail_bb = irwriter_label(w);
  int32_t cached_ok_bb = irwriter_label(w);
  irwriter_br_cond(w, irwriter_icmp(w, "eq", "i32", slot_reg, irwriter_imm(w, "-2")), cached_fail_bb, cached_ok_bb);

  irwriter_bb_at(w, cached_fail_bb);
  irwriter_ret(w, "i32", irwriter_imm(w, "-1"));

  irwriter_bb_at(w, cached_ok_bb);

  int32_t n_alts = 0;
  BranchAltInfo* alts = NULL;
  int32_t* branch_ids = NULL;

  if (has_br) {
    alts = _build_branch_alts(cl, rule_id, &n_alts);

    int32_t cached_return_bb = irwriter_label(w);

    for (int32_t i = 0; i < n_alts; i++) {
      if (!alts[i].is_rule_ref) {
        continue;
      }
      int32_t read_bb = irwriter_label(w);
      int32_t next_bb = irwriter_label(w);
      irwriter_br_cond(w, irwriter_icmp(w, "eq", "i32", slot_reg, irwriter_imm_int(w, alts[i].slot_val)), read_bb,
                       next_bb);
      irwriter_bb_at(w, read_bb);
      IrVal child_len = peg_ir_memo_get(w, col_type_ref, "%table", "%col", 0, alts[i].child_slot);
      irwriter_store(w, "i32", child_len, len_ptr);
      irwriter_br(w, cached_return_bb);
      irwriter_bb_at(w, next_bb);
    }

    IrVal neg_slot = irwriter_binop(w, "sub", "i32", irwriter_imm(w, "0"), slot_reg);
    IrVal minus3 = irwriter_binop(w, "sub", "i32", neg_slot, irwriter_imm(w, "3"));
    IrVal decoded_len = irwriter_binop(w, "and", "i32", minus3, irwriter_imm(w, "65535"));
    irwriter_store(w, "i32", decoded_len, len_ptr);
    irwriter_br(w, cached_return_bb);

    irwriter_bb_at(w, cached_return_bb);
    irwriter_ret(w, "i32", irwriter_load(w, "i32", len_ptr));
  } else {
    irwriter_ret(w, "i32", slot_reg);
  }

  // Compute path
  irwriter_bb_at(w, compute_bb);

  if (has_br && alts) {
    branch_ids = malloc((size_t)n_alts * sizeof(int32_t));
    for (int32_t i = 0; i < n_alts; i++) {
      branch_ids[i] = alts[i].slot_val;
    }
  }

  IrVal match_len =
      peg_ir_gen_rule_body(w, cl->rules, rule_id, col_type_ref, branch_ids, n_alts, fail_bb, slot_val_ptr);
  IrVal slot_val = irwriter_load(w, "i32", slot_val_ptr);
  peg_ir_memo_set(w, col_type_ref, "%table", "%col", 0, (int32_t)cl->rules[rule_id].slot_index, slot_val);
  irwriter_ret(w, "i32", match_len);

  irwriter_bb_at(w, fail_bb);
  peg_ir_memo_set(w, col_type_ref, "%table", "%col", 0, (int32_t)cl->rules[rule_id].slot_index, irwriter_imm(w, "-2"));
  irwriter_ret(w, "i32", irwriter_imm(w, "-1"));

  irwriter_define_end(w);
  free(branch_ids);
  free(alts);
}

static void _gen_rule_shared(int32_t rule_id, ScopeClosure* cl, IrWriter* w) {
  ScopedRule* sr = &cl->rules[rule_id];
  char col_type_ref[72];
  _gen_rule_prologue(sr->name, cl, w, col_type_ref, sizeof(col_type_ref));

  IrVal slot_val_ptr = irwriter_alloca(w, "i32");
  int32_t has_br = _has_branches(cl, rule_id);
  IrVal len_ptr = has_br ? irwriter_alloca(w, "i32") : -1;

  int32_t check_slot_bb = irwriter_label(w);
  int32_t compute_bb = irwriter_label(w);
  int32_t match_fail_bb = irwriter_label(w);
  int32_t bit_fail_bb = irwriter_label(w);

  irwriter_br_cond(w,
                   peg_ir_bit_test(w, col_type_ref, "%table", "%col", (int32_t)cl->rules[rule_id].segment_index,
                                   (int32_t)cl->rules[rule_id].rule_bit_mask),
                   check_slot_bb, bit_fail_bb);

  irwriter_bb_at(w, check_slot_bb);
  IrVal slot_reg = peg_ir_memo_get(w, col_type_ref, "%table", "%col", 1, (int32_t)cl->rules[rule_id].slot_index);

  int32_t not_uncached_bb = irwriter_label(w);
  irwriter_br_cond(w, irwriter_icmp(w, "ne", "i32", slot_reg, irwriter_imm(w, "-1")), not_uncached_bb, compute_bb);

  irwriter_bb_at(w, not_uncached_bb);

  int32_t n_alts = 0;
  BranchAltInfo* alts = NULL;
  int32_t* branch_ids = NULL;

  if (has_br) {
    alts = _build_branch_alts(cl, rule_id, &n_alts);

    int32_t cached_return_bb = irwriter_label(w);

    for (int32_t i = 0; i < n_alts; i++) {
      if (!alts[i].is_rule_ref) {
        continue;
      }
      int32_t read_bb = irwriter_label(w);
      int32_t next_bb = irwriter_label(w);
      irwriter_br_cond(w, irwriter_icmp(w, "eq", "i32", slot_reg, irwriter_imm_int(w, alts[i].slot_val)), read_bb,
                       next_bb);
      irwriter_bb_at(w, read_bb);
      IrVal child_len = peg_ir_memo_get(w, col_type_ref, "%table", "%col", 1, alts[i].child_slot);
      irwriter_store(w, "i32", child_len, len_ptr);
      irwriter_br(w, cached_return_bb);
      irwriter_bb_at(w, next_bb);
    }

    IrVal neg_slot = irwriter_binop(w, "sub", "i32", irwriter_imm(w, "0"), slot_reg);
    IrVal minus3 = irwriter_binop(w, "sub", "i32", neg_slot, irwriter_imm(w, "3"));
    IrVal decoded_len = irwriter_binop(w, "and", "i32", minus3, irwriter_imm(w, "65535"));
    irwriter_store(w, "i32", decoded_len, len_ptr);
    irwriter_br(w, cached_return_bb);

    irwriter_bb_at(w, cached_return_bb);
    irwriter_ret(w, "i32", irwriter_load(w, "i32", len_ptr));
  } else {
    irwriter_ret(w, "i32", slot_reg);
  }

  // Compute path
  irwriter_bb_at(w, compute_bb);

  if (has_br && alts) {
    branch_ids = malloc((size_t)n_alts * sizeof(int32_t));
    for (int32_t i = 0; i < n_alts; i++) {
      branch_ids[i] = alts[i].slot_val;
    }
  }

  IrVal match_len =
      peg_ir_gen_rule_body(w, cl->rules, rule_id, col_type_ref, branch_ids, n_alts, match_fail_bb, slot_val_ptr);

  peg_ir_bit_exclude(w, col_type_ref, "%table", "%col", (int32_t)cl->rules[rule_id].segment_index,
                     (int32_t)cl->rules[rule_id].rule_bit_mask);
  IrVal slot_val = irwriter_load(w, "i32", slot_val_ptr);
  peg_ir_memo_set(w, col_type_ref, "%table", "%col", 1, (int32_t)cl->rules[rule_id].slot_index, slot_val);
  irwriter_ret(w, "i32", match_len);

  irwriter_bb_at(w, match_fail_bb);
  peg_ir_bit_deny(w, col_type_ref, "%table", "%col", (int32_t)cl->rules[rule_id].segment_index,
                  (int32_t)cl->rules[rule_id].rule_bit_mask);
  irwriter_ret(w, "i32", irwriter_imm(w, "-1"));

  irwriter_bb_at(w, bit_fail_bb);
  irwriter_ret(w, "i32", irwriter_imm(w, "-1"));

  irwriter_define_end(w);
  free(branch_ids);
  free(alts);
}

// --- Public API ---

void peg_gen(PegGenInput* input, HeaderWriter* hw, IrWriter* w, bool compress_memoize, const char* prefix) {
  PegRule* rules = input->rules;
  int32_t n_rules = (int32_t)darray_size(rules);
  if (n_rules == 0) {
    return;
  }

  Symtab tokens = {0};
  symtab_init(&tokens, 0);
  Symtab analysis_symbols = {0};
  symtab_init(&analysis_symbols, 0);

  ScopeClosure* closures = _build_closures(rules, n_rules, &tokens);
  int32_t n_closures = (int32_t)darray_size(closures);

  // Assign scoped_rule_id and run nullable/first/last analysis
  for (int32_t si = 0; si < n_closures; si++) {
    ScopeClosure* cl = &closures[si];
    int32_t n_defined = symtab_count(&cl->defined_rules);
    for (int32_t j = 0; j < n_defined; j++) {
      cl->rules[j].scoped_rule_id = (uint32_t)j;
    }
    _analyze_closure(cl, &analysis_symbols);
  }

  // --- Header: PegRef + node types ---
  _gen_ref_type(hw);
  for (int32_t i = 0; i < n_rules; i++) {
    _gen_node_type(hw, &rules[i]);
  }
  hw_blank(hw);

  // --- Assign slots and build Col types ---
  if (!compress_memoize) {
    for (int32_t si = 0; si < n_closures; si++) {
      ScopeClosure* cl = &closures[si];
      int32_t n_defined = symtab_count(&cl->defined_rules);
      cl->n_bits = 0;
      cl->n_slots = n_defined;
      snprintf(cl->col_type, sizeof(cl->col_type), "Col.%s", cl->scope_name);
      snprintf(cl->hdr_col_type, sizeof(cl->hdr_col_type), "Col_%s", cl->scope_name);
      for (int32_t j = 0; j < n_defined; j++) {
        cl->rules[j].slot_index = (uint32_t)j;
      }
      _gen_col_type_naive(hw, cl);
    }
  } else {
    for (int32_t si = 0; si < n_closures; si++) {
      ScopeClosure* cl = &closures[si];
      int32_t n_defined = symtab_count(&cl->defined_rules);
      snprintf(cl->col_type, sizeof(cl->col_type), "Col.%s", cl->scope_name);
      snprintf(cl->hdr_col_type, sizeof(cl->hdr_col_type), "Col_%s", cl->scope_name);

      Graph* g = _build_interference_graph(cl->rules, n_defined);
      int32_t* edges = graph_edges(g);
      int32_t n_edges = graph_n_edges(g);
      ColoringResult* cr = coloring_solve(n_defined, edges, n_edges, n_defined, 1000000, 42);
      int32_t max_color = -1;

      cl->n_bits = coloring_get_sg_size(cr);
      for (int32_t j = 0; j < n_defined; j++) {
        int32_t sg_id, seg_mask;
        coloring_get_segment_info(cr, j, &sg_id, &seg_mask);
        cl->rules[j].segment_index = (uint32_t)sg_id;
        cl->rules[j].rule_bit_mask = (uint32_t)seg_mask;
        cl->rules[j].slot_index = (uint32_t)sg_id;
        if (sg_id > max_color) {
          max_color = sg_id;
        }
      }

      // Compute segment_mask (OR of all rule_bit_masks sharing the same segment_index)
      for (int32_t j = 0; j < n_defined; j++) {
        uint32_t seg = cl->rules[j].segment_index;
        uint32_t mask = 0;
        for (int32_t k = 0; k < n_defined; k++) {
          if (cl->rules[k].segment_index == seg) {
            mask |= cl->rules[k].rule_bit_mask;
          }
        }
        cl->rules[j].segment_mask = mask;
      }

      cl->n_slots = max_color + 1;

      coloring_result_del(cr);
      graph_del(g);

      _gen_col_type_shared(hw, cl);
    }
  }

  // --- Header: utility functions ---
  hw_blank(hw);
  hw_raw(hw, "static inline bool peg_has_next(PegRef ref) { return ref.next_col >= 0; }\n");
  hw_raw(hw, "static inline PegRef peg_get_next(PegRef ref) { return (PegRef){ref.table, ref.next_col, -1}; }\n");
  hw_blank(hw);

  // --- Header: load functions ---
  for (int32_t i = 0; i < n_rules; i++) {
    const char* sn = _scope_name(&rules[i]);
    int32_t si = _closure_index(closures, sn);
    int32_t rid = symtab_find(&closures[si].defined_rules, rules[i].name);
    if (rid >= 0) {
      _gen_load_impl(hw, &rules[i], rid, &closures[si], prefix);
    }
  }

  // --- IR: extern declarations and backtrack stack ---
  peg_ir_declare_externs(w);
  peg_ir_emit_bt_defs(w);

  // --- IR: per-scope Col type definitions ---
  for (int32_t si = 0; si < n_closures; si++) {
    _define_col_type_ir(w, &closures[si]);
  }

  // --- IR: token ID legend ---
  {
    int32_t n_tok = symtab_count(&tokens);
    if (n_tok > 0) {
      irwriter_rawf(w, "\n; Token IDs:\n");
      for (int32_t t = 0; t < n_tok; t++) {
        irwriter_rawf(w, ";   %d = @%s\n", t, symtab_get(&tokens, t));
      }
    }
  }

  // --- IR: slot mapping legend ---
  for (int32_t si = 0; si < n_closures; si++) {
    ScopeClosure* cl = &closures[si];
    int32_t n_defined = symtab_count(&cl->defined_rules);
    irwriter_rawf(w, "\n; Slot map (scope %s, %d slots):\n", cl->scope_name, cl->n_slots);
    for (int32_t s = 0; s < cl->n_slots; s++) {
      irwriter_rawf(w, ";   slot %d:", s);
      int first = 1;
      for (int32_t j = 0; j < n_defined; j++) {
        if ((int32_t)cl->rules[j].slot_index == s) {
          if (cl->n_bits > 0) {
            irwriter_rawf(w, "%s %s(bit 0x%x)", first ? "" : " |", symtab_get(&cl->defined_rules, j),
                          cl->rules[j].rule_bit_mask);
          } else {
            irwriter_rawf(w, "%s %s", first ? "" : " |", symtab_get(&cl->defined_rules, j));
          }
          first = 0;
        }
      }
      irwriter_rawf(w, "\n");
    }
  }
  irwriter_rawf(w, "\n");

  // --- IR: rule functions ---
  for (int32_t i = 0; i < n_rules; i++) {
    const char* sn = _scope_name(&rules[i]);
    int32_t si = _closure_index(closures, sn);
    int32_t rid = symtab_find(&closures[si].defined_rules, rules[i].name);
    if (rid < 0) {
      continue;
    }
    if (!compress_memoize) {
      _gen_rule_naive(rid, &closures[si], w);
    } else {
      _gen_rule_shared(rid, &closures[si], w);
    }
  }

  // --- Cleanup ---
  symtab_free(&tokens);
  symtab_free(&analysis_symbols);
  _free_closures(closures);
}
