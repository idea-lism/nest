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

// --- Per-rule analysis data ---

typedef struct {
  Bitset* first_set;
  Bitset* last_set;
  int32_t slot_idx;
  int32_t sg_id;
  int32_t seg_mask;
  int32_t has_branches;
  int32_t scope_idx;
  char* scope;
  PegRule* rule;
} RuleInfo;

typedef struct {
  PegRule rule;
  int32_t source_idx;
} AnalysisRule;

// --- Scope info: rules grouped by scope ---

typedef struct {
  char* scope_name;
  int32_t* rule_indices;
  int32_t n_rules;
  int32_t n_slots;
  int32_t n_bits;
  char col_type[64];     // LLVM IR type name (e.g. "Col.main")
  char hdr_col_type[64]; // C header type name (e.g. "Col_main")
} ScopeCtx;

// --- Analysis helpers ---

static char* _dup_str(const char* s) { return s ? strdup(s) : NULL; }

static PegUnit _clone_unit(PegUnit* src) {
  PegUnit out = {
      .kind = src->kind,
      .name = _dup_str(src->name),
      .multiplier = src->multiplier,
      .tag = _dup_str(src->tag),
      .children = darray_new(sizeof(PegUnit), 0),
      .ninterlace = src->ninterlace,
  };

  if (src->interlace) {
    out.interlace = malloc(sizeof(PegUnit));
    *out.interlace = _clone_unit(src->interlace);
  }

  for (int32_t i = 0; i < (int32_t)darray_size(src->children); i++) {
    PegUnit child = _clone_unit(&src->children[i]);
    darray_push(out.children, child);
  }

  return out;
}

static void _free_unit(PegUnit* unit) {
  free(unit->name);
  free(unit->tag);
  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    _free_unit(&unit->children[i]);
  }
  darray_del(unit->children);
  if (unit->interlace) {
    _free_unit(unit->interlace);
    free(unit->interlace);
  }
}

static PegRule _clone_rule(PegRule* src) {
  return (PegRule){
      .name = _dup_str(src->name),
      .seq = _clone_unit(&src->seq),
      .scope = _dup_str(src->scope),
  };
}

static void _free_analysis_rules(AnalysisRule* rules) {
  for (int32_t i = 0; i < (int32_t)darray_size(rules); i++) {
    free(rules[i].rule.name);
    free(rules[i].rule.scope);
    _free_unit(&rules[i].rule.seq);
  }
  darray_del(rules);
}

static int32_t _symbol_id(Symtab* st, const char* kind, const char* name) {
  char key[256];
  snprintf(key, sizeof(key), "%s:%s", kind, name);
  return symtab_intern(st, key);
}

static int32_t _scope_symbol_id(Symtab* st, const char* name) { return _symbol_id(st, "scope", name); }

static int32_t _token_id_for_analysis(Symtab* st, const char* name) { return _symbol_id(st, "tok", name); }

// --- Scope helpers ---

static const char* _scope_name(PegRule* rule) { return (rule->scope && rule->scope[0]) ? rule->scope : "main"; }

static int32_t _is_scope_entry(PegRule* rule) { return rule->scope && rule->scope[0]; }

static int32_t _scope_index(ScopeCtx* scopes, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    if (strcmp(scopes[i].scope_name, name) == 0) {
      return i;
    }
  }
  return -1;
}

static int32_t _rule_index_by_name(PegRule* rules, int32_t n_rules, const char* name) {
  for (int32_t i = 0; i < n_rules; i++) {
    if (strcmp(rules[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

static void _walk_closure(PegUnit* unit, PegRule* rules, int32_t n_rules, Bitset* visited, int32_t** out_indices) {
  if (unit->kind == PEG_ID) {
    int32_t ri = _rule_index_by_name(rules, n_rules, unit->name);
    if (ri >= 0 && !bitset_contains(visited, (uint32_t)ri)) {
      if (_is_scope_entry(&rules[ri])) {
        return; // don't expand into other scopes
      }
      bitset_add_bit(visited, (uint32_t)ri);
      darray_push(*out_indices, ri);
      _walk_closure(&rules[ri].seq, rules, n_rules, visited, out_indices);
    }
    return;
  }
  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    _walk_closure(&unit->children[i], rules, n_rules, visited, out_indices);
  }
  if (unit->interlace) {
    _walk_closure(unit->interlace, rules, n_rules, visited, out_indices);
  }
}

static ScopeCtx* _gather_scope_closures(PegRule* rules, int32_t n_rules) {
  ScopeCtx* scopes = darray_new(sizeof(ScopeCtx), 0);

  // Seed each scope with its explicitly-tagged rules.
  for (int32_t i = 0; i < n_rules; i++) {
    const char* name = _scope_name(&rules[i]);
    int32_t si = _scope_index(scopes, name);
    if (si < 0) {
      ScopeCtx scope = {0};
      scope.scope_name = strdup(name);
      scope.rule_indices = darray_new(sizeof(int32_t), 0);
      darray_push(scopes, scope);
      si = (int32_t)darray_size(scopes) - 1;
    }
    darray_push(scopes[si].rule_indices, i);
  }

  // Expand each scope's closure by recursively walking rule bodies.
  for (int32_t si = 0; si < (int32_t)darray_size(scopes); si++) {
    Bitset* visited = bitset_new();
    int32_t seed_count = (int32_t)darray_size(scopes[si].rule_indices);
    for (int32_t j = 0; j < seed_count; j++) {
      bitset_add_bit(visited, (uint32_t)scopes[si].rule_indices[j]);
    }
    // Walk each seed rule's body to discover transitive sub-rule deps.
    for (int32_t j = 0; j < seed_count; j++) {
      _walk_closure(&rules[scopes[si].rule_indices[j]].seq, rules, n_rules, visited, &scopes[si].rule_indices);
    }
    scopes[si].n_rules = (int32_t)darray_size(scopes[si].rule_indices);
    bitset_del(visited);
  }

  return scopes;
}

static void _free_scopes(ScopeCtx* scopes) {
  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    free(scopes[i].scope_name);
    darray_del(scopes[i].rule_indices);
  }
  darray_del(scopes);
}

// --- First/last set computation ---
// is_first: true = first set (children[0]), false = last set (children[n-1])

static int32_t _analysis_rule_index(AnalysisRule* rules, int32_t n_rules, const char* name) {
  for (int32_t i = 0; i < n_rules; i++) {
    if (strcmp(rules[i].rule.name, name) == 0) {
      return i;
    }
  }
  return -1;
}

static void _breakdown_unit(PegUnit* unit, const char* owner_name, int32_t* next_id, AnalysisRule** out_rules) {
  // Capture fields before any darray_push on *out_rules, because unit may
  // point into that same array and reallocation would invalidate it.
  PegUnitKind kind = unit->kind;
  PegUnit* children = unit->children;
  PegUnit* interlace = unit->interlace;

  if (kind == PEG_SEQ) {
    for (int32_t i = 0; i < (int32_t)darray_size(children); i++) {
      PegUnit* child = &children[i];
      if (child->kind == PEG_BRANCHES) {
        int32_t name_len = snprintf(NULL, 0, "%s$%d", owner_name, *next_id) + 1;
        char synthetic_name[name_len];
        snprintf(synthetic_name, (size_t)name_len, "%s$%d", owner_name, *next_id);
        (*next_id)++;

        AnalysisRule synthetic = {
            .rule =
                {
                    .name = strdup(synthetic_name),
                    .seq = _clone_unit(child),
                    .scope = NULL,
                },
            .source_idx = -1,
        };
        darray_push(*out_rules, synthetic);
        _breakdown_unit(&(*out_rules)[darray_size(*out_rules) - 1].rule.seq, synthetic_name, next_id, out_rules);

        _free_unit(child);
        *child = (PegUnit){
            .kind = PEG_ID,
            .name = strdup(synthetic_name),
        };
      } else {
        _breakdown_unit(child, owner_name, next_id, out_rules);
      }
    }
  } else {
    for (int32_t i = 0; i < (int32_t)darray_size(children); i++) {
      _breakdown_unit(&children[i], owner_name, next_id, out_rules);
    }
  }

  if (interlace) {
    _breakdown_unit(interlace, owner_name, next_id, out_rules);
  }
}

static AnalysisRule* _build_analysis_rules(PegRule* rules, int32_t n_rules) {
  AnalysisRule* analysis_rules = darray_new(sizeof(AnalysisRule), 0);
  for (int32_t i = 0; i < n_rules; i++) {
    AnalysisRule ar = {
        .rule = _clone_rule(&rules[i]),
        .source_idx = i,
    };
    darray_push(analysis_rules, ar);
  }

  for (int32_t i = 0; i < n_rules; i++) {
    int32_t next_id = 1;
    _breakdown_unit(&analysis_rules[i].rule.seq, analysis_rules[i].rule.name, &next_id, &analysis_rules);
  }

  return analysis_rules;
}

static void _compute_set(PegUnit* unit, Bitset* out, AnalysisRule* rules, int32_t n_rules, Bitset* visited,
                         Symtab* symbols, int32_t is_first) {
  if (unit->kind == PEG_TOK) {
    bitset_add_bit(out, (uint32_t)_token_id_for_analysis(symbols, unit->name));
  } else if (unit->kind == PEG_ID) {
    int32_t ri = _analysis_rule_index(rules, n_rules, unit->name);
    if (ri >= 0) {
      if (_is_scope_entry(&rules[ri].rule)) {
        bitset_add_bit(out, (uint32_t)_scope_symbol_id(symbols, rules[ri].rule.name));
      } else if (!bitset_contains(visited, (uint32_t)ri)) {
        bitset_add_bit(visited, (uint32_t)ri);
        _compute_set(&rules[ri].rule.seq, out, rules, n_rules, visited, symbols, is_first);
      }
    }
  } else if (unit->kind == PEG_SEQ) {
    int32_t n = (int32_t)darray_size(unit->children);
    if (n > 0) {
      _compute_set(&unit->children[is_first ? 0 : n - 1], out, rules, n_rules, visited, symbols, is_first);
    }
  } else if (unit->kind == PEG_BRANCHES) {
    int32_t n = (int32_t)darray_size(unit->children);
    for (int32_t i = 0; i < n; i++) {
      _compute_set(&unit->children[i], out, rules, n_rules, visited, symbols, is_first);
    }
  }
}

static int32_t _are_exclusive(RuleInfo* a, RuleInfo* b) {
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

static Graph* _build_scope_interference_graph(RuleInfo* rule_infos, int32_t* rule_indices, int32_t n_rules) {
  Graph* g = graph_new(n_rules);
  for (int32_t i = 0; i < n_rules; i++) {
    for (int32_t j = i + 1; j < n_rules; j++) {
      if (!_are_exclusive(&rule_infos[rule_indices[i]], &rule_infos[rule_indices[j]])) {
        graph_add_edge(g, i, j);
      }
    }
  }
  return g;
}

// --- Header generation helpers ---

static PegUnit** _collect_branches(PegRule* rule) {
  PegUnit** all_branches = darray_new(sizeof(PegUnit*), 0);
  for (int32_t i = 0; i < (int32_t)darray_size(rule->seq.children); i++) {
    if (rule->seq.children[i].kind == PEG_BRANCHES) {
      PegUnit* bu = &rule->seq.children[i];
      for (int32_t j = 0; j < (int32_t)darray_size(bu->children); j++) {
        darray_push(all_branches, &bu->children[j]);
      }
    }
  }
  return all_branches;
}

static void _make_struct_name(PegRule* rule, char* out, int32_t out_size) {
  snprintf(out, (size_t)out_size, "%sNode", rule->name);
  out[0] = (char)toupper((unsigned char)out[0]);
}

// --- Header generation ---

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

static void _gen_node_type(HeaderWriter* hw, PegRule* rule) {
  char struct_name[128];
  _make_struct_name(rule, struct_name, sizeof(struct_name));

  hw_struct_begin(hw, struct_name);

  PegUnit** all_branches = _collect_branches(rule);
  int32_t nbranches = (int32_t)darray_size(all_branches);

  if (nbranches > 0) {
    hw_raw(hw, "  struct {\n");
    for (int32_t i = 0; i < nbranches; i++) {
      PegUnit* branch = all_branches[i];
      if (branch->tag && branch->tag[0]) {
        hw_fmt(hw, "    bool %s : 1;\n", branch->tag);
      } else {
        hw_fmt(hw, "    bool branch%d : 1;\n", i);
      }
    }
    hw_raw(hw, "  } is;\n");

    for (int32_t i = 0; i < nbranches; i++) {
      PegUnit* branch = all_branches[i];
      for (int32_t j = 0; j < (int32_t)darray_size(branch->children); j++) {
        PegUnit* child = &branch->children[j];
        if (child->name && child->name[0]) {
          hw_field(hw, "PegRef", child->name);
        }
      }
    }
  } else {
    for (int32_t i = 0; i < (int32_t)darray_size(rule->seq.children); i++) {
      PegUnit* child = &rule->seq.children[i];
      if (child->name && child->name[0]) {
        hw_field(hw, "PegRef", child->name);
      }
    }
  }

  darray_del(all_branches);
  hw_struct_end(hw);
  hw_fmt(hw, " %s;\n\n", struct_name);
}

// --- Per-scope Col type in header ---

static void _gen_col_type_naive(HeaderWriter* hw, ScopeCtx* scope) {
  char name[128];
  snprintf(name, sizeof(name), "Col_%s", scope->scope_name);
  int32_t n = scope->n_slots > 0 ? scope->n_slots : 1;
  hw_struct_begin(hw, name);
  hw_fmt(hw, "  int32_t slots[%d];\n", n);
  hw_fmt(hw, "  int32_t aux[%d];\n", n);
  hw_struct_end(hw);
  hw_fmt(hw, " %s;\n\n", name);
}

static void _gen_col_type_shared(HeaderWriter* hw, ScopeCtx* scope) {
  char name[128];
  snprintf(name, sizeof(name), "Col_%s", scope->scope_name);
  int32_t n = scope->n_slots > 0 ? scope->n_slots : 1;
  hw_struct_begin(hw, name);
  hw_fmt(hw, "  int32_t bits[%d];\n", scope->n_bits);
  hw_fmt(hw, "  int32_t slots[%d];\n", n);
  hw_fmt(hw, "  int32_t aux[%d];\n", n);
  hw_struct_end(hw);
  hw_fmt(hw, " %s;\n\n", name);
}

// --- LLVM IR Col type definition ---

static void _define_col_type_ir(IrWriter* w, ScopeCtx* scope) {
  int32_t n = scope->n_slots > 0 ? scope->n_slots : 1;
  if (scope->n_bits > 0) {
    irwriter_rawf(w, "%%%s = type { [%d x i32], [%d x i32], [%d x i32] }\n", scope->col_type, scope->n_bits, n, n);
  } else {
    irwriter_rawf(w, "%%%s = type { [%d x i32], [%d x i32] }\n", scope->col_type, n, n);
  }
}

// --- Load function generation ---

static void _emit_child_load(HeaderWriter* hw, PegUnit* child, const char* cur_var, int32_t indent) {
  const char* sp = indent >= 2 ? "    " : "  ";
  const char* inner = indent >= 2 ? "      " : "    ";
  const char* var = (child->name && child->name[0]) ? child->name : NULL;

  if (var) {
    hw_fmt(hw, "%snode.%s = (PegRef){table, %s, -1};\n", sp, var, cur_var);
  }

  if (child->kind == PEG_ID) {
    if (var) {
      hw_fmt(hw, "%s{ int32_t l = parse_%s((void*)table, %s);\n", sp, var, cur_var);
      hw_fmt(hw, "%sif (l < 0) l = 0;\n", inner);
      if (child->multiplier == '+' || child->multiplier == '*') {
        hw_fmt(hw, "%snode.%s.next_col = %s + l;\n", inner, var, cur_var);
      }
      hw_fmt(hw, "%s%s += l; }\n", inner, cur_var);
    } else {
      hw_fmt(hw, "%s{ int32_t l = parse_%s((void*)table, %s);\n", sp, child->name, cur_var);
      hw_fmt(hw, "%sif (l < 0) l = 0;\n", inner);
      hw_fmt(hw, "%s%s += l; }\n", inner, cur_var);
    }
  } else if (child->kind == PEG_TOK) {
    hw_fmt(hw, "%s%s += 1;\n", sp, cur_var);
  }
}

static void _gen_load_impl(HeaderWriter* hw, PegRule* rule, RuleInfo* ri, ScopeCtx* scopes) {
  char struct_name[128];
  _make_struct_name(rule, struct_name, sizeof(struct_name));
  const char* col_type = scopes[ri->scope_idx].hdr_col_type;

  int32_t fn_len = snprintf(NULL, 0, "load_%s", rule->name) + 1;
  char func_name[fn_len];
  snprintf(func_name, (size_t)fn_len, "load_%s", rule->name);

  hw_blank(hw);
  hw_fmt(hw, "static inline %s %s(PegRef ref) {\n", struct_name, func_name);
  hw_fmt(hw, "  %s node = {0};\n", struct_name);
  hw_fmt(hw, "  %s* table = (%s*)ref.table;\n", col_type, col_type);
  hw_fmt(hw, "  int32_t col = ref.col;\n");
  hw_fmt(hw, "  int32_t cur = col;\n");

  PegUnit** all_branches = _collect_branches(rule);
  int32_t nbranches = (int32_t)darray_size(all_branches);

  if (nbranches > 0) {
    hw_fmt(hw, "  int32_t branch_id = table[col].aux[%d];\n", ri->slot_idx);

    int32_t branch_idx = 0;
    for (int32_t i = 0; i < (int32_t)darray_size(rule->seq.children); i++) {
      PegUnit* child = &rule->seq.children[i];

      if (child->kind == PEG_BRANCHES) {
        int32_t bn = (int32_t)darray_size(child->children);
        for (int32_t b = 0; b < bn; b++) {
          int32_t bid = branch_idx + b + 1;
          PegUnit* branch = &child->children[b];
          const char* tag = (branch->tag && branch->tag[0]) ? branch->tag : NULL;
          if (tag) {
            hw_fmt(hw, "  node.is.%s = (branch_id == %d);\n", tag, bid);
          } else {
            hw_fmt(hw, "  node.is.branch%d = (branch_id == %d);\n", branch_idx + b, bid);
          }

          hw_fmt(hw, "  if (branch_id == %d) {\n", bid);
          hw_fmt(hw, "    int32_t bcur = cur;\n");
          for (int32_t j = 0; j < (int32_t)darray_size(branch->children); j++) {
            _emit_child_load(hw, &branch->children[j], "bcur", 2);
          }
          hw_fmt(hw, "  }\n");
        }
        branch_idx += bn;
      } else {
        _emit_child_load(hw, child, "cur", 1);
      }
    }
  } else {
    for (int32_t i = 0; i < (int32_t)darray_size(rule->seq.children); i++) {
      _emit_child_load(hw, &rule->seq.children[i], "cur", 1);
    }
  }

  darray_del(all_branches);
  hw_raw(hw, "  return node;\n");
  hw_raw(hw, "}\n");
}

// --- Rule function generation ---

static void _gen_rule_prologue(PegRule* rule, ScopeCtx* scope, IrWriter* w, char* col_type_ref,
                               int32_t col_type_ref_size) {
  const char* args[] = {"i8*", "i32"};
  const char* arg_names[] = {"table", "col"};

  char func_name[128];
  snprintf(func_name, sizeof(func_name), "parse_%s", rule->name);

  irwriter_define_start(w, func_name, "i32", 2, args, arg_names);
  irwriter_bb(w);

  snprintf(col_type_ref, (size_t)col_type_ref_size, "%%%s", scope->col_type);
}

static void _gen_rule_naive(PegRule* rule, RuleInfo* ri, ScopeCtx* scope, IrWriter* w, Symtab* tokens) {
  char col_type_ref[72];
  _gen_rule_prologue(rule, scope, w, col_type_ref, sizeof(col_type_ref));

  int32_t cached_bb = irwriter_label(w);
  int32_t cached_ok_bb = irwriter_label(w);
  int32_t cached_fail_bb = irwriter_label(w);
  int32_t compute_bb = irwriter_label(w);
  int32_t fail_bb = irwriter_label(w);

  IrVal slot_reg = peg_ir_memo_get(w, col_type_ref, "%table", "%col", 0, ri->slot_idx);
  irwriter_br_cond(w, irwriter_icmp(w, "ne", "i32", slot_reg, irwriter_imm(w, "-1")), cached_bb, compute_bb);

  irwriter_bb_at(w, cached_bb);
  irwriter_br_cond(w, irwriter_icmp(w, "eq", "i32", slot_reg, irwriter_imm(w, "-2")), cached_fail_bb, cached_ok_bb);

  irwriter_bb_at(w, cached_ok_bb);
  irwriter_ret(w, "i32", slot_reg);

  irwriter_bb_at(w, cached_fail_bb);
  irwriter_ret(w, "i32", irwriter_imm(w, "-1"));

  irwriter_bb_at(w, compute_bb);
  IrVal packed = peg_ir_gen_rule_body(w, &rule->seq, tokens, col_type_ref, ri->has_branches, fail_bb);

  if (ri->has_branches) {
    IrVal match_len = irwriter_binop(w, "and", "i32", packed, irwriter_imm(w, "65535"));
    IrVal branch_id = irwriter_binop(w, "lshr", "i32", packed, irwriter_imm(w, "16"));
    peg_ir_memo_set(w, col_type_ref, "%table", "%col", 0, ri->slot_idx, match_len);
    peg_ir_memo_set(w, col_type_ref, "%table", "%col", 1, ri->slot_idx, branch_id);
    irwriter_ret(w, "i32", match_len);
  } else {
    peg_ir_memo_set(w, col_type_ref, "%table", "%col", 0, ri->slot_idx, packed);
    irwriter_ret(w, "i32", packed);
  }

  irwriter_bb_at(w, fail_bb);
  peg_ir_memo_set(w, col_type_ref, "%table", "%col", 0, ri->slot_idx, irwriter_imm(w, "-2"));
  irwriter_ret(w, "i32", irwriter_imm(w, "-1"));

  irwriter_define_end(w);
}

static void _gen_rule_shared(PegRule* rule, RuleInfo* ri, ScopeCtx* scope, IrWriter* w, Symtab* tokens) {
  char col_type_ref[72];
  _gen_rule_prologue(rule, scope, w, col_type_ref, sizeof(col_type_ref));

  int32_t check_slot_bb = irwriter_label(w);
  int32_t cached_bb = irwriter_label(w);
  int32_t compute_bb = irwriter_label(w);
  int32_t match_fail_bb = irwriter_label(w);
  int32_t bit_fail_bb = irwriter_label(w);

  irwriter_br_cond(w, peg_ir_bit_test(w, col_type_ref, "%table", "%col", ri->sg_id, ri->seg_mask), check_slot_bb,
                   bit_fail_bb);

  irwriter_bb_at(w, check_slot_bb);
  IrVal slot_reg = peg_ir_memo_get(w, col_type_ref, "%table", "%col", 1, ri->slot_idx);
  irwriter_br_cond(w, irwriter_icmp(w, "ne", "i32", slot_reg, irwriter_imm(w, "-1")), cached_bb, compute_bb);

  irwriter_bb_at(w, cached_bb);
  irwriter_ret(w, "i32", slot_reg);

  irwriter_bb_at(w, compute_bb);
  IrVal packed = peg_ir_gen_rule_body(w, &rule->seq, tokens, col_type_ref, ri->has_branches, match_fail_bb);

  peg_ir_bit_exclude(w, col_type_ref, "%table", "%col", ri->sg_id, ri->seg_mask);
  if (ri->has_branches) {
    IrVal match_len = irwriter_binop(w, "and", "i32", packed, irwriter_imm(w, "65535"));
    IrVal branch_id = irwriter_binop(w, "lshr", "i32", packed, irwriter_imm(w, "16"));
    peg_ir_memo_set(w, col_type_ref, "%table", "%col", 1, ri->slot_idx, match_len);
    peg_ir_memo_set(w, col_type_ref, "%table", "%col", 2, ri->slot_idx, branch_id);
    irwriter_ret(w, "i32", match_len);
  } else {
    peg_ir_memo_set(w, col_type_ref, "%table", "%col", 1, ri->slot_idx, packed);
    irwriter_ret(w, "i32", packed);
  }

  irwriter_bb_at(w, match_fail_bb);
  peg_ir_bit_deny(w, col_type_ref, "%table", "%col", ri->sg_id, ri->seg_mask);
  irwriter_ret(w, "i32", irwriter_imm(w, "-1"));

  irwriter_bb_at(w, bit_fail_bb);
  irwriter_ret(w, "i32", irwriter_imm(w, "-1"));

  irwriter_define_end(w);
}

// --- Public API ---

void peg_gen(PegGenInput* input, HeaderWriter* hw, IrWriter* w) {
  PegRule* rules = input->rules;
  int32_t n_rules = (int32_t)darray_size(rules);

  if (n_rules == 0) {
    return;
  }

  Symtab analysis_symbols = {0};
  Symtab tokens = {0};
  ScopeCtx* scopes = _gather_scope_closures(rules, n_rules);
  AnalysisRule* analysis_rules = _build_analysis_rules(rules, n_rules);

  RuleInfo* rule_infos = calloc((size_t)n_rules, sizeof(RuleInfo));
  for (int32_t i = 0; i < n_rules; i++) {
    rule_infos[i].first_set = bitset_new();
    rule_infos[i].last_set = bitset_new();
    rule_infos[i].rule = &rules[i];
    PegUnit** br = _collect_branches(&rules[i]);
    rule_infos[i].has_branches = (int32_t)darray_size(br) > 0;
    darray_del(br);
  }

  for (int32_t i = 0; i < n_rules; i++) {
    Bitset* visited = bitset_new();
    _compute_set(&analysis_rules[i].rule.seq, rule_infos[i].first_set, analysis_rules,
                 (int32_t)darray_size(analysis_rules), visited, &analysis_symbols, 1);
    bitset_del(visited);

    visited = bitset_new();
    _compute_set(&analysis_rules[i].rule.seq, rule_infos[i].last_set, analysis_rules,
                 (int32_t)darray_size(analysis_rules), visited, &analysis_symbols, 0);
    bitset_del(visited);
  }

  // --- Header: PegRef + node types ---
  _gen_ref_type(hw);
  for (int32_t i = 0; i < n_rules; i++) {
    _gen_node_type(hw, &rules[i]);
  }

  hw_blank(hw);

  // --- Assign per-scope slot indices and build Col types ---
  if (input->mode == PEG_MODE_NAIVE) {
    for (int32_t si = 0; si < (int32_t)darray_size(scopes); si++) {
      ScopeCtx* scope = &scopes[si];
      scope->n_bits = 0;
      scope->n_slots = scope->n_rules;
      snprintf(scope->col_type, sizeof(scope->col_type), "Col.%s", scope->scope_name);
      snprintf(scope->hdr_col_type, sizeof(scope->hdr_col_type), "Col_%s", scope->scope_name);

      for (int32_t j = 0; j < scope->n_rules; j++) {
        int32_t ri = scope->rule_indices[j];
        rule_infos[ri].slot_idx = j;
        rule_infos[ri].scope_idx = si;
      }
    }

    for (int32_t si = 0; si < (int32_t)darray_size(scopes); si++) {
      _gen_col_type_naive(hw, &scopes[si]);
    }
  } else {
    // Row-shared mode: graph coloring
    for (int32_t si = 0; si < (int32_t)darray_size(scopes); si++) {
      ScopeCtx* scope = &scopes[si];
      snprintf(scope->col_type, sizeof(scope->col_type), "Col.%s", scope->scope_name);
      snprintf(scope->hdr_col_type, sizeof(scope->hdr_col_type), "Col_%s", scope->scope_name);

      Graph* g = _build_scope_interference_graph(rule_infos, scope->rule_indices, scope->n_rules);
      int32_t* edges = graph_edges(g);
      int32_t n_edges = graph_n_edges(g);
      ColoringResult* cr = coloring_solve(scope->n_rules, edges, n_edges, scope->n_rules, 1000000, 42);
      int32_t max_color = -1;

      scope->n_bits = coloring_get_sg_size(cr);
      for (int32_t j = 0; j < scope->n_rules; j++) {
        int32_t global_ri = scope->rule_indices[j];
        coloring_get_segment_info(cr, j, &rule_infos[global_ri].sg_id, &rule_infos[global_ri].seg_mask);
        rule_infos[global_ri].slot_idx = rule_infos[global_ri].sg_id;
        rule_infos[global_ri].scope_idx = si;
        if (rule_infos[global_ri].sg_id > max_color) {
          max_color = rule_infos[global_ri].sg_id;
        }
      }

      scope->n_slots = max_color + 1;

      coloring_result_del(cr);
      graph_del(g);
    }

    for (int32_t si = 0; si < (int32_t)darray_size(scopes); si++) {
      _gen_col_type_shared(hw, &scopes[si]);
    }
  }

  // --- Header: utility functions ---
  hw_blank(hw);
  hw_raw(hw, "static inline bool peg_has_next(PegRef ref) { return ref.next_col >= 0; }\n");
  hw_raw(hw, "static inline PegRef peg_get_next(PegRef ref) { return (PegRef){ref.table, ref.next_col, -1}; }\n");
  hw_blank(hw);
  hw_raw(hw, "#include <stdlib.h>\n");
  hw_raw(hw, "#include <string.h>\n");
  hw_blank(hw);

  for (int32_t si = 0; si < (int32_t)darray_size(scopes); si++) {
    ScopeCtx* scope = &scopes[si];
    hw_fmt(hw, "static inline %s* peg_alloc_%s(int32_t n_cols) {\n", scope->hdr_col_type, scope->scope_name);
    hw_fmt(hw, "  %s* table = (%s*)malloc(sizeof(%s) * n_cols);\n", scope->hdr_col_type, scope->hdr_col_type,
           scope->hdr_col_type);
    hw_fmt(hw, "  if (table) memset(table, 0xFF, sizeof(%s) * n_cols);\n", scope->hdr_col_type);
    hw_fmt(hw, "  return table;\n");
    hw_fmt(hw, "}\n\n");
    hw_fmt(hw, "static inline void peg_free_%s(%s* table) { free(table); }\n\n", scope->scope_name,
           scope->hdr_col_type);
  }

  // --- Header: parse function declarations (before load impls that call them) ---
  for (int32_t i = 0; i < n_rules; i++) {
    hw_fmt(hw, "int32_t parse_%s(void* table, int32_t col);\n", rules[i].name);
  }
  hw_blank(hw);

  for (int32_t i = 0; i < n_rules; i++) {
    _gen_load_impl(hw, &rules[i], &rule_infos[i], scopes);
  }

  // --- IR: extern declarations and backtrack stack definitions ---
  peg_ir_declare_externs(w);
  peg_ir_emit_bt_defs(w);

  // --- IR: per-scope Col type definitions ---
  for (int32_t si = 0; si < (int32_t)darray_size(scopes); si++) {
    _define_col_type_ir(w, &scopes[si]);
  }

  // --- IR: rule functions ---
  if (input->mode == PEG_MODE_NAIVE) {
    for (int32_t i = 0; i < n_rules; i++) {
      const char* sn = _scope_name(&rules[i]);
      int32_t si = _scope_index(scopes, sn);
      _gen_rule_naive(&rules[i], &rule_infos[i], &scopes[si], w, &tokens);
    }
  } else {
    for (int32_t i = 0; i < n_rules; i++) {
      const char* sn = _scope_name(&rules[i]);
      int32_t si = _scope_index(scopes, sn);
      _gen_rule_shared(&rules[i], &rule_infos[i], &scopes[si], w, &tokens);
    }
  }

  // --- Cleanup ---
  for (int32_t i = 0; i < n_rules; i++) {
    bitset_del(rule_infos[i].first_set);
    bitset_del(rule_infos[i].last_set);
  }
  symtab_free(&analysis_symbols);
  symtab_free(&tokens);
  _free_analysis_rules(analysis_rules);
  _free_scopes(scopes);
  free(rule_infos);
}
