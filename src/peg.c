// PEG (Parsing Expression Grammar) code generation.
// Generates packrat parser functions in LLVM IR,
// and emits node type definitions to the C header.
// Follows the PEG IR reference patterns from peg_ir.md.

#include "peg.h"
#include "bitset.h"
#include "coloring.h"
#include "darray.h"
#include "graph.h"
#include "peg_ir.h"

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
  int32_t seg_full_mask;
  char* scope;
  PegRule* rule;
} RuleInfo;

// --- Scope info: rules grouped by scope ---

typedef struct {
  char* scope_name;
  int32_t* rule_indices;
  int32_t n_rules;
  int32_t n_slots;
  int32_t n_bits;
  char col_type[64];
} ScopeCtx;

// --- Token name registry ---

static int32_t _token_id(char** tokens, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(tokens); i++) {
    if (strcmp(tokens[i], name) == 0) {
      return i + 1;
    }
  }
  darray_push(tokens, strdup(name));
  return (int32_t)darray_size(tokens);
}

// --- Scope helpers ---

static const char* _scope_name(PegRule* rule) { return (rule->scope && rule->scope[0]) ? rule->scope : "main"; }

static int32_t _scope_index(ScopeCtx* scopes, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    if (strcmp(scopes[i].scope_name, name) == 0) {
      return i;
    }
  }
  return -1;
}

static ScopeCtx* _collect_scopes(PegRule* rules, int32_t n_rules) {
  ScopeCtx* scopes = darray_new(sizeof(ScopeCtx), 0);
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
    scopes[si].n_rules = (int32_t)darray_size(scopes[si].rule_indices);
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

static void _compute_set(PegUnit* unit, Bitset* out, RuleInfo* rule_infos, int32_t n_rules, Bitset* visited,
                         char** tokens, int32_t is_first) {
  if (unit->kind == PEG_TOK) {
    bitset_add_bit(out, (uint32_t)_token_id(tokens, unit->name));
  } else if (unit->kind == PEG_ID) {
    for (int32_t i = 0; i < n_rules; i++) {
      if (rule_infos[i].rule && strcmp(rule_infos[i].rule->name, unit->name) == 0) {
        if (!bitset_contains(visited, (uint32_t)i)) {
          bitset_add_bit(visited, (uint32_t)i);
          _compute_set(&rule_infos[i].rule->seq, out, rule_infos, n_rules, visited, tokens, is_first);
        }
        break;
      }
    }
  } else if (unit->kind == PEG_SEQ) {
    int32_t n = (int32_t)darray_size(unit->children);
    if (n > 0) {
      _compute_set(&unit->children[is_first ? 0 : n - 1], out, rule_infos, n_rules, visited, tokens, is_first);
    }
  } else if (unit->kind == PEG_BRANCHES) {
    int32_t n = (int32_t)darray_size(unit->children);
    for (int32_t i = 0; i < n; i++) {
      _compute_set(&unit->children[i], out, rule_infos, n_rules, visited, tokens, is_first);
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

static Graph* _build_interference_graph(RuleInfo* rule_infos, int32_t n_rules) {
  Graph* g = graph_new(n_rules);
  for (int32_t i = 0; i < n_rules; i++) {
    for (int32_t j = i + 1; j < n_rules; j++) {
      if (strcmp(_scope_name(rule_infos[i].rule), _scope_name(rule_infos[j].rule)) != 0) {
        continue;
      }
      if (!_are_exclusive(&rule_infos[i], &rule_infos[j])) {
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
  if (out[0] >= 'a' && out[0] <= 'z') {
    out[0] -= 32;
  }
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
  hw_struct_begin(hw, "Col");
  hw_fmt(hw, "  int32_t slots[%d];\n", scope->n_slots > 0 ? scope->n_slots : 1);
  hw_struct_end(hw);
  hw_raw(hw, " Col;\n\n");
}

static void _gen_col_type_shared(HeaderWriter* hw, ScopeCtx* scope) {
  hw_struct_begin(hw, "Col");
  hw_fmt(hw, "  int32_t bits[%d];\n", scope->n_bits);
  hw_fmt(hw, "  int32_t slots[%d];\n", scope->n_slots > 0 ? scope->n_slots : 1);
  hw_struct_end(hw);
  hw_raw(hw, " Col;\n\n");
}

// --- LLVM IR Col type definition ---

static void _define_col_type_ir(IrWriter* w, ScopeCtx* scope) {
  char body[128];
  if (scope->n_bits > 0) {
    snprintf(body, sizeof(body), "{ [%d x i32], [%d x i32] }", scope->n_bits, scope->n_slots > 0 ? scope->n_slots : 1);
  } else {
    snprintf(body, sizeof(body), "{ [%d x i32] }", scope->n_slots > 0 ? scope->n_slots : 1);
  }
  irwriter_type_def(w, scope->col_type, body);
}

// --- Load function generation ---

static void _gen_load_impl(HeaderWriter* hw, PegRule* rule) {
  char struct_name[128];
  _make_struct_name(rule, struct_name, sizeof(struct_name));

  int32_t fn_len = snprintf(NULL, 0, "load_%s", rule->name) + 1;
  char func_name[fn_len];
  snprintf(func_name, (size_t)fn_len, "load_%s", rule->name);

  hw_blank(hw);
  hw_fmt(hw, "static inline %s %s(PegRef ref) {\n", struct_name, func_name);
  hw_fmt(hw, "  %s node = {0};\n", struct_name);
  hw_fmt(hw, "  Col* table = (Col*)ref.table;\n");
  hw_fmt(hw, "  int32_t col = ref.col;\n");
  hw_fmt(hw, "  int32_t cur = col;\n");

  PegUnit** all_branches = _collect_branches(rule);
  int32_t nbranches = (int32_t)darray_size(all_branches);

  if (nbranches > 0) {
    hw_raw(hw, "  (void)table;\n");
    hw_raw(hw, "  (void)cur;\n");
  } else {
    for (int32_t i = 0; i < (int32_t)darray_size(rule->seq.children); i++) {
      PegUnit* child = &rule->seq.children[i];
      if (child->name && child->name[0]) {
        hw_fmt(hw, "  node.%s = (PegRef){table, cur, -1};\n", child->name);
      }

      if (child->kind == PEG_ID && child->name && child->name[0]) {
        hw_fmt(hw, "  int32_t len_%d = parse_%s((void*)table, cur);\n", i, child->name);
        hw_fmt(hw, "  if (len_%d < 0) len_%d = 0;\n", i, i);
        if (child->multiplier == '+' || child->multiplier == '*') {
          hw_fmt(hw, "  node.%s.next_col = cur + len_%d;\n", child->name, i);
        }
        hw_fmt(hw, "  cur += len_%d;\n", i);
      } else if (child->kind == PEG_ID) {
        hw_fmt(hw, "  int32_t len_%d = parse_%s((void*)table, cur);\n", i, child->name);
        hw_fmt(hw, "  if (len_%d < 0) len_%d = 0;\n", i, i);
        hw_fmt(hw, "  cur += len_%d;\n", i);
      } else if (child->kind == PEG_TOK) {
        hw_raw(hw, "  cur += 1;\n");
      }
    }
  }

  darray_del(all_branches);
  hw_raw(hw, "  return node;\n");
  hw_raw(hw, "}\n");
}

// --- IR code generation context ---

typedef struct {
  IrWriter* w;
  char** tokens;
  const char* col_type;
  int32_t bt_stack;
} GenCtx;

// --- Leaf call: emit call to match_tok or parse_rule, return register number ---

static int32_t _emit_leaf_call(GenCtx* ctx, PegUnit* unit, const char* col_expr) {
  if (unit->kind == PEG_ID) {
    return peg_ir_call(ctx->w, unit->name, "%table", col_expr);
  }
  if (unit->kind == PEG_TOK) {
    int32_t tok_id = _token_id(ctx->tokens, unit->name);
    char tok_buf[16];
    snprintf(tok_buf, sizeof(tok_buf), "%d", tok_id);
    return peg_ir_tok(ctx->w, tok_buf, col_expr);
  }
  return irwriter_imm(ctx->w, "i32", -1);
}

// Convert a string SSA value to a register number (emits add 0, optimized away by LLVM).
static int32_t _to_reg(GenCtx* ctx, const char* val) {
  return irwriter_param(ctx->w, "i32", val);
}

// Format a register number as a string operand.
static void _fmt_reg(char* out, int32_t out_size, int32_t reg) {
  snprintf(out, (size_t)out_size, "%%r%d", reg);
}

static int32_t _gen_ir(GenCtx* ctx, PegUnit* unit, const char* col_expr, int32_t fail_label);

static int32_t _gen_empty(GenCtx* ctx) { return irwriter_imm(ctx->w, "i32", 0); }

static int32_t _gen_leaf(GenCtx* ctx, PegUnit* unit, const char* col_expr, int32_t fail_label) {
  int32_t r = _emit_leaf_call(ctx, unit, col_expr);

  int32_t ok = irwriter_label(ctx->w);
  irwriter_br_cond_r(ctx->w, irwriter_icmp_imm(ctx->w, "slt", "i32", r, 0), fail_label, ok);
  irwriter_bb_at(ctx->w, ok);

  return r;
}

static int32_t _gen_optional(GenCtx* ctx, PegUnit* unit, const char* col_expr) {
  int32_t try_bb = irwriter_label(ctx->w);
  int32_t miss_bb = irwriter_label(ctx->w);
  int32_t done_bb = irwriter_label(ctx->w);

  int32_t r = _emit_leaf_call(ctx, unit, col_expr);
  int32_t zero = _gen_empty(ctx);

  irwriter_br_cond_r(ctx->w, irwriter_icmp_imm(ctx->w, "slt", "i32", r, 0), miss_bb, try_bb);

  irwriter_bb_at(ctx->w, try_bb);
  irwriter_br(ctx->w, done_bb);

  irwriter_bb_at(ctx->w, miss_bb);
  irwriter_br(ctx->w, done_bb);

  irwriter_bb_at(ctx->w, done_bb);
  return irwriter_phi2(ctx->w, "i32", r, try_bb, zero, miss_bb);
}

static int32_t _gen_greedy_loop(GenCtx* ctx, PegUnit* unit, const char* col_expr, int32_t acc_ptr, int32_t loop_bb,
                                int32_t body_bb, int32_t end_bb) {
  irwriter_bb_at(ctx->w, loop_bb);
  int32_t cur_acc = irwriter_load(ctx->w, "i32", acc_ptr);
  int32_t next_col = irwriter_binop(ctx->w, "add", "i32", _to_reg(ctx, col_expr), cur_acc);

  char next_col_s[16];
  _fmt_reg(next_col_s, sizeof(next_col_s), next_col);
  int32_t r = _emit_leaf_call(ctx, unit, next_col_s);
  irwriter_br_cond_r(ctx->w, irwriter_icmp_imm(ctx->w, "slt", "i32", r, 0), end_bb, body_bb);

  irwriter_bb_at(ctx->w, body_bb);
  int32_t prev = irwriter_load(ctx->w, "i32", acc_ptr);
  int32_t next = irwriter_binop(ctx->w, "add", "i32", prev, r);
  irwriter_store(ctx->w, "i32", next, acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  irwriter_bb_at(ctx->w, end_bb);
  return irwriter_load(ctx->w, "i32", acc_ptr);
}

static int32_t _gen_plus(GenCtx* ctx, PegUnit* unit, const char* col_expr, int32_t fail_label) {
  int32_t ok_bb = irwriter_label(ctx->w);
  int32_t loop_bb = irwriter_label(ctx->w);
  int32_t body_bb = irwriter_label(ctx->w);
  int32_t end_bb = irwriter_label(ctx->w);

  int32_t acc_ptr = irwriter_alloca(ctx->w, "i32");
  irwriter_store_imm(ctx->w, "i32", 0, acc_ptr);

  int32_t first = _emit_leaf_call(ctx, unit, col_expr);
  irwriter_br_cond_r(ctx->w, irwriter_icmp_imm(ctx->w, "slt", "i32", first, 0), fail_label, ok_bb);

  irwriter_bb_at(ctx->w, ok_bb);
  irwriter_store(ctx->w, "i32", first, acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  return _gen_greedy_loop(ctx, unit, col_expr, acc_ptr, loop_bb, body_bb, end_bb);
}

static int32_t _gen_star(GenCtx* ctx, PegUnit* unit, const char* col_expr) {
  int32_t loop_bb = irwriter_label(ctx->w);
  int32_t body_bb = irwriter_label(ctx->w);
  int32_t end_bb = irwriter_label(ctx->w);

  int32_t acc_ptr = irwriter_alloca(ctx->w, "i32");
  irwriter_store_imm(ctx->w, "i32", 0, acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  return _gen_greedy_loop(ctx, unit, col_expr, acc_ptr, loop_bb, body_bb, end_bb);
}

static int32_t _gen_interlace_loop(GenCtx* ctx, PegUnit* unit, PegUnit* sep, const char* col_expr, int32_t acc_ptr,
                                   int32_t loop_bb, int32_t sep_ok_bb, int32_t body_bb, int32_t end_bb) {
  irwriter_bb_at(ctx->w, loop_bb);
  int32_t cur_acc = irwriter_load(ctx->w, "i32", acc_ptr);
  int32_t cur_col = irwriter_binop(ctx->w, "add", "i32", _to_reg(ctx, col_expr), cur_acc);

  char cur_col_s[16];
  _fmt_reg(cur_col_s, sizeof(cur_col_s), cur_col);
  int32_t sr = _emit_leaf_call(ctx, sep, cur_col_s);
  irwriter_br_cond_r(ctx->w, irwriter_icmp_imm(ctx->w, "slt", "i32", sr, 0), end_bb, sep_ok_bb);

  irwriter_bb_at(ctx->w, sep_ok_bb);
  int32_t after_sep = irwriter_binop(ctx->w, "add", "i32", cur_col, sr);
  char after_sep_s[16];
  _fmt_reg(after_sep_s, sizeof(after_sep_s), after_sep);
  int32_t er = _emit_leaf_call(ctx, unit, after_sep_s);
  irwriter_br_cond_r(ctx->w, irwriter_icmp_imm(ctx->w, "slt", "i32", er, 0), end_bb, body_bb);

  irwriter_bb_at(ctx->w, body_bb);
  int32_t prev_acc = irwriter_load(ctx->w, "i32", acc_ptr);
  int32_t sep_elem = irwriter_binop(ctx->w, "add", "i32", sr, er);
  int32_t next = irwriter_binop(ctx->w, "add", "i32", prev_acc, sep_elem);
  irwriter_store(ctx->w, "i32", next, acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  irwriter_bb_at(ctx->w, end_bb);
  return irwriter_load(ctx->w, "i32", acc_ptr);
}

static int32_t _gen_plus_interlace(GenCtx* ctx, PegUnit* unit, PegUnit* sep, const char* col_expr,
                                   int32_t fail_label) {
  int32_t first_ok_bb = irwriter_label(ctx->w);
  int32_t loop_bb = irwriter_label(ctx->w);
  int32_t sep_ok_bb = irwriter_label(ctx->w);
  int32_t body_bb = irwriter_label(ctx->w);
  int32_t end_bb = irwriter_label(ctx->w);

  int32_t acc_ptr = irwriter_alloca(ctx->w, "i32");
  irwriter_store_imm(ctx->w, "i32", 0, acc_ptr);

  int32_t first = _emit_leaf_call(ctx, unit, col_expr);
  irwriter_br_cond_r(ctx->w, irwriter_icmp_imm(ctx->w, "slt", "i32", first, 0), fail_label, first_ok_bb);

  irwriter_bb_at(ctx->w, first_ok_bb);
  irwriter_store(ctx->w, "i32", first, acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  return _gen_interlace_loop(ctx, unit, sep, col_expr, acc_ptr, loop_bb, sep_ok_bb, body_bb, end_bb);
}

static int32_t _gen_star_interlace(GenCtx* ctx, PegUnit* unit, PegUnit* sep, const char* col_expr) {
  int32_t first_ok_bb = irwriter_label(ctx->w);
  int32_t loop_bb = irwriter_label(ctx->w);
  int32_t sep_ok_bb = irwriter_label(ctx->w);
  int32_t body_bb = irwriter_label(ctx->w);
  int32_t end_bb = irwriter_label(ctx->w);
  int32_t empty_bb = irwriter_label(ctx->w);

  int32_t acc_ptr = irwriter_alloca(ctx->w, "i32");
  irwriter_store_imm(ctx->w, "i32", 0, acc_ptr);

  int32_t first = _emit_leaf_call(ctx, unit, col_expr);
  irwriter_br_cond_r(ctx->w, irwriter_icmp_imm(ctx->w, "slt", "i32", first, 0), empty_bb, first_ok_bb);

  irwriter_bb_at(ctx->w, first_ok_bb);
  irwriter_store(ctx->w, "i32", first, acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  int32_t result = _gen_interlace_loop(ctx, unit, sep, col_expr, acc_ptr, loop_bb, sep_ok_bb, body_bb, end_bb);

  irwriter_bb_at(ctx->w, empty_bb);
  irwriter_br(ctx->w, end_bb);

  return result;
}

// --- Main gen() dispatcher ---

static int32_t _gen_ir(GenCtx* ctx, PegUnit* unit, const char* col_expr, int32_t fail_label) {
  if (unit->kind == PEG_TOK || unit->kind == PEG_ID) {
    if (unit->multiplier == 0) {
      return _gen_leaf(ctx, unit, col_expr, fail_label);
    }
    if (unit->multiplier == '?') {
      return _gen_optional(ctx, unit, col_expr);
    }
    if (unit->multiplier == '+') {
      if (unit->interlace && unit->ninterlace > 0) {
        return _gen_plus_interlace(ctx, unit, unit->interlace, col_expr, fail_label);
      }
      return _gen_plus(ctx, unit, col_expr, fail_label);
    }
    if (unit->multiplier == '*') {
      if (unit->interlace && unit->ninterlace > 0) {
        return _gen_star_interlace(ctx, unit, unit->interlace, col_expr);
      }
      return _gen_star(ctx, unit, col_expr);
    }
  }

  if (unit->kind == PEG_SEQ) {
    int32_t n = (int32_t)darray_size(unit->children);
    if (n == 0) {
      return _gen_empty(ctx);
    }

    int32_t total_ptr = irwriter_alloca(ctx->w, "i32");
    irwriter_store_imm(ctx->w, "i32", 0, total_ptr);

    for (int32_t i = 0; i < n; i++) {
      int32_t prev = irwriter_load(ctx->w, "i32", total_ptr);
      int32_t child_col = irwriter_binop(ctx->w, "add", "i32", _to_reg(ctx, col_expr), prev);

      char child_col_s[16];
      _fmt_reg(child_col_s, sizeof(child_col_s), child_col);
      int32_t child_len = _gen_ir(ctx, &unit->children[i], child_col_s, fail_label);

      int32_t new_total = irwriter_load(ctx->w, "i32", total_ptr);
      int32_t updated = irwriter_binop(ctx->w, "add", "i32", new_total, child_len);
      irwriter_store(ctx->w, "i32", updated, total_ptr);
    }
    return irwriter_load(ctx->w, "i32", total_ptr);
  }

  if (unit->kind == PEG_BRANCHES) {
    int32_t n = (int32_t)darray_size(unit->children);
    if (n == 0) {
      return _gen_empty(ctx);
    }

    int32_t done_bb = irwriter_label(ctx->w);
    int32_t result_ptr = irwriter_alloca(ctx->w, "i32");

    peg_ir_backtrack_push(ctx->w, ctx->bt_stack, col_expr);

    for (int32_t i = 0; i < n; i++) {
      int32_t is_last = (i == n - 1);
      int32_t alt_bb = is_last ? -1 : irwriter_label(ctx->w);
      int32_t ft = is_last ? fail_label : alt_bb;

      int32_t r = _gen_ir(ctx, &unit->children[i], col_expr, ft);

      peg_ir_backtrack_pop(ctx->w, ctx->bt_stack);
      irwriter_store(ctx->w, "i32", r, result_ptr);
      irwriter_br(ctx->w, done_bb);

      if (!is_last) {
        irwriter_bb_at(ctx->w, alt_bb);
        int32_t restored = peg_ir_backtrack_restore(ctx->w, ctx->bt_stack);
        peg_ir_backtrack_pop(ctx->w, ctx->bt_stack);
        char restored_s[16];
        _fmt_reg(restored_s, sizeof(restored_s), restored);
        peg_ir_backtrack_push(ctx->w, ctx->bt_stack, restored_s);
      }
    }

    irwriter_bb_at(ctx->w, done_bb);
    return irwriter_load(ctx->w, "i32", result_ptr);
  }

  return _gen_empty(ctx);
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

static int32_t _gen_rule_compute(PegRule* rule, IrWriter* w, char** tokens, const char* col_type_ref,
                                 int32_t fail_label) {
  int32_t bt_stack = irwriter_alloca(w, "%BtStack");
  int32_t bt_top_ptr = irwriter_gep(w, "%BtStack", bt_stack, "i32 0, i32 1");
  irwriter_store_imm(w, "i32", -1, bt_top_ptr);

  GenCtx ctx = {.w = w, .tokens = tokens, .col_type = col_type_ref, .bt_stack = bt_stack};
  return _gen_ir(&ctx, &rule->seq, "%col", fail_label);
}

static void _gen_rule_naive(PegRule* rule, RuleInfo* ri, ScopeCtx* scope, IrWriter* w, char** tokens) {
  char col_type_ref[72];
  _gen_rule_prologue(rule, scope, w, col_type_ref, sizeof(col_type_ref));

  int32_t cached_bb = irwriter_label(w);
  int32_t compute_bb = irwriter_label(w);
  int32_t fail_bb = irwriter_label(w);

  int32_t slot_reg = peg_ir_memo_get(w, col_type_ref, "%table", "%col", 0, ri->slot_idx);
  irwriter_br_cond_r(w, irwriter_icmp_imm(w, "ne", "i32", slot_reg, -1), cached_bb, compute_bb);

  irwriter_bb_at(w, cached_bb);
  irwriter_ret(w, "i32", slot_reg);

  irwriter_bb_at(w, compute_bb);
  int32_t match_len = _gen_rule_compute(rule, w, tokens, col_type_ref, fail_bb);

  peg_ir_memo_set(w, col_type_ref, "%table", "%col", 0, ri->slot_idx, match_len);
  irwriter_ret(w, "i32", match_len);

  irwriter_bb_at(w, fail_bb);
  int32_t neg1 = irwriter_imm(w, "i32", -1);
  peg_ir_memo_set(w, col_type_ref, "%table", "%col", 0, ri->slot_idx, neg1);
  irwriter_ret_i(w, "i32", -1);

  irwriter_define_end(w);
}

static void _gen_rule_shared(PegRule* rule, RuleInfo* ri, ScopeCtx* scope, IrWriter* w, char** tokens) {
  char col_type_ref[72];
  _gen_rule_prologue(rule, scope, w, col_type_ref, sizeof(col_type_ref));

  int32_t check_slot_bb = irwriter_label(w);
  int32_t cached_bb = irwriter_label(w);
  int32_t compute_bb = irwriter_label(w);
  int32_t match_fail_bb = irwriter_label(w);
  int32_t bit_fail_bb = irwriter_label(w);

  irwriter_br_cond_r(w, peg_ir_bit_test(w, col_type_ref, "%table", "%col", ri->sg_id, ri->seg_mask), check_slot_bb,
                     bit_fail_bb);

  irwriter_bb_at(w, check_slot_bb);
  int32_t slot_reg = peg_ir_memo_get(w, col_type_ref, "%table", "%col", 1, ri->slot_idx);
  irwriter_br_cond_r(w, irwriter_icmp_imm(w, "ne", "i32", slot_reg, -1), cached_bb, compute_bb);

  irwriter_bb_at(w, cached_bb);
  irwriter_ret(w, "i32", slot_reg);

  irwriter_bb_at(w, compute_bb);
  int32_t match_len = _gen_rule_compute(rule, w, tokens, col_type_ref, match_fail_bb);

  peg_ir_bit_exclude(w, col_type_ref, "%table", "%col", ri->sg_id, ri->seg_mask);
  peg_ir_memo_set(w, col_type_ref, "%table", "%col", 1, ri->slot_idx, match_len);
  irwriter_ret(w, "i32", match_len);

  irwriter_bb_at(w, match_fail_bb);
  peg_ir_bit_deny(w, col_type_ref, "%table", "%col", ri->sg_id, ri->seg_mask);
  irwriter_ret_i(w, "i32", -1);

  irwriter_bb_at(w, bit_fail_bb);
  irwriter_ret_i(w, "i32", -1);

  irwriter_define_end(w);
}

// --- Public API ---

void peg_gen(PegGenInput* input, HeaderWriter* hw, IrWriter* w) {
  PegRule* rules = input->rules;
  int32_t n_rules = (int32_t)darray_size(rules);

  if (n_rules == 0) {
    return;
  }

  char** tokens = darray_new(sizeof(char*), 0);
  ScopeCtx* scopes = _collect_scopes(rules, n_rules);

  RuleInfo* rule_infos = calloc((size_t)n_rules, sizeof(RuleInfo));
  for (int32_t i = 0; i < n_rules; i++) {
    rule_infos[i].first_set = bitset_new();
    rule_infos[i].last_set = bitset_new();
    rule_infos[i].rule = &rules[i];
  }

  for (int32_t i = 0; i < n_rules; i++) {
    Bitset* visited = bitset_new();
    _compute_set(&rules[i].seq, rule_infos[i].first_set, rule_infos, n_rules, visited, tokens, 1);
    bitset_del(visited);

    visited = bitset_new();
    _compute_set(&rules[i].seq, rule_infos[i].last_set, rule_infos, n_rules, visited, tokens, 0);
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

      for (int32_t j = 0; j < scope->n_rules; j++) {
        int32_t ri = scope->rule_indices[j];
        rule_infos[ri].slot_idx = j;
      }
    }

    // Use the first scope's layout for the C Col type (all scopes may differ,
    // but generate one Col with the max slots for the C header)
    int32_t max_slots = 0;
    for (int32_t si = 0; si < (int32_t)darray_size(scopes); si++) {
      if (scopes[si].n_slots > max_slots) {
        max_slots = scopes[si].n_slots;
      }
    }
    ScopeCtx hdr_scope = {.n_bits = 0, .n_slots = max_slots};
    _gen_col_type_naive(hw, &hdr_scope);
  } else {
    // Row-shared mode: graph coloring
    Graph* g = _build_interference_graph(rule_infos, n_rules);
    int32_t* edges = graph_edges(g);
    int32_t n_edges = graph_n_edges(g);

    ColoringResult* cr = coloring_solve(n_rules, edges, n_edges, n_rules, 1000000, 42);
    int32_t sg_size = coloring_get_sg_size(cr);

    int32_t max_color = 0;
    for (int32_t i = 0; i < n_rules; i++) {
      coloring_get_segment_info(cr, i, &rule_infos[i].sg_id, &rule_infos[i].seg_mask);
      rule_infos[i].slot_idx = rule_infos[i].sg_id;
      if (rule_infos[i].sg_id > max_color) {
        max_color = rule_infos[i].sg_id;
      }
    }

    for (int32_t i = 0; i < n_rules; i++) {
      int32_t full = 0;
      for (int32_t j = 0; j < n_rules; j++) {
        if (rule_infos[i].sg_id == rule_infos[j].sg_id &&
            strcmp(_scope_name(rule_infos[i].rule), _scope_name(rule_infos[j].rule)) == 0) {
          full |= rule_infos[j].seg_mask;
        }
      }
      rule_infos[i].seg_full_mask = full;
    }

    for (int32_t si = 0; si < (int32_t)darray_size(scopes); si++) {
      ScopeCtx* scope = &scopes[si];
      scope->n_bits = sg_size;
      scope->n_slots = max_color + 1;
      snprintf(scope->col_type, sizeof(scope->col_type), "Col.%s", scope->scope_name);
    }

    ScopeCtx hdr_scope = {.n_bits = sg_size, .n_slots = max_color + 1};
    _gen_col_type_shared(hw, &hdr_scope);

    coloring_result_del(cr);
    graph_del(g);
  }

  // --- Header: utility functions ---
  hw_blank(hw);
  hw_raw(hw, "static inline bool peg_has_next(PegRef ref) { return ref.next_col >= 0; }\n");
  hw_raw(hw, "static inline PegRef peg_get_next(PegRef ref) { return (PegRef){ref.table, ref.next_col, -1}; }\n");
  hw_blank(hw);
  hw_raw(hw, "#include <stdlib.h>\n");
  hw_raw(hw, "#include <string.h>\n");
  hw_blank(hw);
  hw_fmt(hw, "static inline Col* peg_alloc_table(int32_t n_cols) {\n");
  hw_fmt(hw, "  Col* table = (Col*)malloc(sizeof(Col) * n_cols);\n");
  hw_fmt(hw, "  if (table) memset(table, 0xFF, sizeof(Col) * n_cols);\n");
  hw_fmt(hw, "  return table;\n");
  hw_fmt(hw, "}\n");
  hw_blank(hw);
  hw_raw(hw, "static inline void peg_free_table(Col* table) { free(table); }\n");

  for (int32_t i = 0; i < n_rules; i++) {
    _gen_load_impl(hw, &rules[i]);
  }

  // --- Header: parse function declarations ---
  for (int32_t i = 0; i < n_rules; i++) {
    hw_fmt(hw, "int32_t parse_%s(void* table, int32_t col);\n", rules[i].name);
  }
  hw_blank(hw);

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
      _gen_rule_naive(&rules[i], &rule_infos[i], &scopes[si], w, tokens);
    }
  } else {
    for (int32_t i = 0; i < n_rules; i++) {
      const char* sn = _scope_name(&rules[i]);
      int32_t si = _scope_index(scopes, sn);
      _gen_rule_shared(&rules[i], &rule_infos[i], &scopes[si], w, tokens);
    }
  }

  // --- Cleanup ---
  for (int32_t i = 0; i < n_rules; i++) {
    bitset_del(rule_infos[i].first_set);
    bitset_del(rule_infos[i].last_set);
  }
  for (int32_t i = 0; i < (int32_t)darray_size(tokens); i++) {
    free(tokens[i]);
  }
  darray_del(tokens);
  _free_scopes(scopes);
  free(rule_infos);
}
