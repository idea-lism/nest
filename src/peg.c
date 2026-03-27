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
  const char* bt_stack;
  int32_t label_counter;
} GenCtx;

// --- Leaf call: emit call to match_tok or parse_rule ---

static void _emit_leaf_call(GenCtx* ctx, PegUnit* unit, const char* col_expr, char* out, int32_t out_size) {
  if (unit->kind == PEG_ID) {
    snprintf(out, (size_t)out_size, "%%r%d", peg_ir_call(ctx->w, unit->name, "%table", col_expr));
    return;
  }
  if (unit->kind == PEG_TOK) {
    int32_t tok_id = _token_id(ctx->tokens, unit->name);
    char tok_buf[16];
    snprintf(tok_buf, sizeof(tok_buf), "%d", tok_id);
    snprintf(out, (size_t)out_size, "%%r%d", peg_ir_tok(ctx->w, tok_buf, col_expr));
    return;
  }
  snprintf(out, (size_t)out_size, "-1");
}

// gen(pattern, col, on_fail): generates IR for matching pattern at col.
// On failure, branches to on_fail. On success, falls through with match length in len_out.
// The caller must ensure the current BB has no terminator before calling.
// After return, the current BB is the success continuation (caller must emit further code or a terminator).

static void _gen_ir(GenCtx* ctx, PegUnit* unit, const char* col_expr, const char* on_fail, char* len_out,
                    int32_t len_out_size);

// gen(empty, col, fail) -> const(0)
static void _gen_empty(GenCtx* ctx, char* len_out, int32_t len_out_size) {
  (void)ctx;
  snprintf(len_out, (size_t)len_out_size, "0");
}

// gen(token, col, fail): r = tok(token, col); fail_if_neg(r, fail); return r
// gen(Rule, col, fail): r = call(Rule, col); fail_if_neg(r, fail); return r
static void _gen_leaf(GenCtx* ctx, PegUnit* unit, const char* col_expr, const char* on_fail, char* len_out,
                      int32_t len_out_size) {
  char call_result[16];
  _emit_leaf_call(ctx, unit, col_expr, call_result, sizeof(call_result));

  char cont[32];
  snprintf(cont, sizeof(cont), "leaf_ok_%d", ctx->label_counter++);
  char cmp[16];
  snprintf(cmp, sizeof(cmp), "%%r%d", irwriter_icmp_imm(ctx->w, "slt", "i32", call_result, 0));
  irwriter_br_cond(ctx->w, cmp, on_fail, cont);
  irwriter_bb(ctx->w, cont);

  snprintf(len_out, (size_t)len_out_size, "%s", call_result);
}

// gen(e?, col, fail): always succeeds
static void _gen_optional(GenCtx* ctx, PegUnit* unit, const char* col_expr, char* len_out, int32_t len_out_size) {
  char try_bb[32], miss_bb[32], done_bb[32];
  snprintf(try_bb, sizeof(try_bb), "opt_try_%d", ctx->label_counter);
  snprintf(miss_bb, sizeof(miss_bb), "opt_miss_%d", ctx->label_counter);
  snprintf(done_bb, sizeof(done_bb), "opt_done_%d", ctx->label_counter);
  ctx->label_counter++;

  char r[16];
  _emit_leaf_call(ctx, unit, col_expr, r, sizeof(r));

  char neg[16];
  snprintf(neg, sizeof(neg), "%%r%d", irwriter_icmp_imm(ctx->w, "slt", "i32", r, 0));
  irwriter_br_cond(ctx->w, neg, miss_bb, try_bb);

  // try_bb: match succeeded
  irwriter_bb(ctx->w, try_bb);
  irwriter_br(ctx->w, done_bb);

  // miss_bb: match failed, result is 0
  irwriter_bb(ctx->w, miss_bb);
  irwriter_br(ctx->w, done_bb);

  irwriter_bb(ctx->w, done_bb);
  snprintf(len_out, (size_t)len_out_size, "%%r%d", irwriter_phi2(ctx->w, "i32", r, try_bb, "0", miss_bb));
}

// Shared greedy loop: load acc, try match at col+acc, accumulate or exit.
// Caller sets up acc_ptr (alloca i32, stored initial value) and branches to loop_bb.
static void _gen_greedy_loop(GenCtx* ctx, PegUnit* unit, const char* col_expr, const char* acc_ptr, const char* loop_bb,
                             const char* body_bb, const char* end_bb, char* len_out, int32_t len_out_size) {
  irwriter_bb(ctx->w, loop_bb);
  char cur_acc[16];
  snprintf(cur_acc, sizeof(cur_acc), "%%r%d", irwriter_load(ctx->w, "i32", acc_ptr));
  char next_col[16];
  snprintf(next_col, sizeof(next_col), "%%r%d", irwriter_binop(ctx->w, "add", "i32", col_expr, cur_acc));

  char r[16];
  _emit_leaf_call(ctx, unit, next_col, r, sizeof(r));
  char neg[16];
  snprintf(neg, sizeof(neg), "%%r%d", irwriter_icmp_imm(ctx->w, "slt", "i32", r, 0));
  irwriter_br_cond(ctx->w, neg, end_bb, body_bb);

  irwriter_bb(ctx->w, body_bb);
  char prev[16];
  snprintf(prev, sizeof(prev), "%%r%d", irwriter_load(ctx->w, "i32", acc_ptr));
  char next[16];
  snprintf(next, sizeof(next), "%%r%d", irwriter_binop(ctx->w, "add", "i32", prev, r));
  irwriter_store(ctx->w, "i32", next, acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  irwriter_bb(ctx->w, end_bb);
  snprintf(len_out, (size_t)len_out_size, "%%r%d", irwriter_load(ctx->w, "i32", acc_ptr));
}

// gen(e+, col, fail): first match required, then loop greedily
static void _gen_plus(GenCtx* ctx, PegUnit* unit, const char* col_expr, const char* on_fail, char* len_out,
                      int32_t len_out_size) {
  int32_t id = ctx->label_counter++;
  char loop_bb[32], body_bb[32], end_bb[32];
  snprintf(loop_bb, sizeof(loop_bb), "plus_loop_%d", id);
  snprintf(body_bb, sizeof(body_bb), "plus_body_%d", id);
  snprintf(end_bb, sizeof(end_bb), "plus_end_%d", id);

  char acc_ptr[16];
  snprintf(acc_ptr, sizeof(acc_ptr), "%%r%d", irwriter_alloca(ctx->w, "i32"));
  irwriter_store(ctx->w, "i32", "0", acc_ptr);

  char first[16];
  _emit_leaf_call(ctx, unit, col_expr, first, sizeof(first));
  char first_ok[32];
  snprintf(first_ok, sizeof(first_ok), "plus_ok_%d", id);
  char cmp[16];
  snprintf(cmp, sizeof(cmp), "%%r%d", irwriter_icmp_imm(ctx->w, "slt", "i32", first, 0));
  irwriter_br_cond(ctx->w, cmp, on_fail, first_ok);

  irwriter_bb(ctx->w, first_ok);
  irwriter_store(ctx->w, "i32", first, acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  _gen_greedy_loop(ctx, unit, col_expr, acc_ptr, loop_bb, body_bb, end_bb, len_out, len_out_size);
}

// gen(e*, col, fail): zero or more, always succeeds
static void _gen_star(GenCtx* ctx, PegUnit* unit, const char* col_expr, char* len_out, int32_t len_out_size) {
  int32_t id = ctx->label_counter++;
  char loop_bb[32], body_bb[32], end_bb[32];
  snprintf(loop_bb, sizeof(loop_bb), "star_loop_%d", id);
  snprintf(body_bb, sizeof(body_bb), "star_body_%d", id);
  snprintf(end_bb, sizeof(end_bb), "star_end_%d", id);

  char acc_ptr[16];
  snprintf(acc_ptr, sizeof(acc_ptr), "%%r%d", irwriter_alloca(ctx->w, "i32"));
  irwriter_store(ctx->w, "i32", "0", acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  _gen_greedy_loop(ctx, unit, col_expr, acc_ptr, loop_bb, body_bb, end_bb, len_out, len_out_size);
}

// gen(e+<sep>, col, fail): e (sep e)* — first element required
// Shared interlace loop: (sep elem)* accumulation.
// Caller sets up acc_ptr, stores first match result, and branches to loop_bb.
static void _gen_interlace_loop(GenCtx* ctx, PegUnit* unit, PegUnit* sep, const char* col_expr, const char* acc_ptr,
                                const char* loop_bb, const char* sep_ok_bb, const char* body_bb, const char* end_bb,
                                char* len_out, int32_t len_out_size) {
  irwriter_bb(ctx->w, loop_bb);
  char cur_acc[16];
  snprintf(cur_acc, sizeof(cur_acc), "%%r%d", irwriter_load(ctx->w, "i32", acc_ptr));
  char cur_col[16];
  snprintf(cur_col, sizeof(cur_col), "%%r%d", irwriter_binop(ctx->w, "add", "i32", col_expr, cur_acc));

  char sr[16];
  _emit_leaf_call(ctx, sep, cur_col, sr, sizeof(sr));
  char sr_neg[16];
  snprintf(sr_neg, sizeof(sr_neg), "%%r%d", irwriter_icmp_imm(ctx->w, "slt", "i32", sr, 0));
  irwriter_br_cond(ctx->w, sr_neg, end_bb, sep_ok_bb);

  irwriter_bb(ctx->w, sep_ok_bb);
  char after_sep[16];
  snprintf(after_sep, sizeof(after_sep), "%%r%d", irwriter_binop(ctx->w, "add", "i32", cur_col, sr));
  char er[16];
  _emit_leaf_call(ctx, unit, after_sep, er, sizeof(er));
  char er_neg[16];
  snprintf(er_neg, sizeof(er_neg), "%%r%d", irwriter_icmp_imm(ctx->w, "slt", "i32", er, 0));
  irwriter_br_cond(ctx->w, er_neg, end_bb, body_bb);

  irwriter_bb(ctx->w, body_bb);
  char prev_acc[16];
  snprintf(prev_acc, sizeof(prev_acc), "%%r%d", irwriter_load(ctx->w, "i32", acc_ptr));
  char sep_elem[16];
  snprintf(sep_elem, sizeof(sep_elem), "%%r%d", irwriter_binop(ctx->w, "add", "i32", sr, er));
  char next[16];
  snprintf(next, sizeof(next), "%%r%d", irwriter_binop(ctx->w, "add", "i32", prev_acc, sep_elem));
  irwriter_store(ctx->w, "i32", next, acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  irwriter_bb(ctx->w, end_bb);
  snprintf(len_out, (size_t)len_out_size, "%%r%d", irwriter_load(ctx->w, "i32", acc_ptr));
}

// gen(e+<sep>, col, fail): e (sep e)* — first element required
static void _gen_plus_interlace(GenCtx* ctx, PegUnit* unit, PegUnit* sep, const char* col_expr, const char* on_fail,
                                char* len_out, int32_t len_out_size) {
  int32_t id = ctx->label_counter++;
  char first_ok_bb[32], loop_bb[32], sep_ok_bb[32], body_bb[32], end_bb[32];
  snprintf(first_ok_bb, sizeof(first_ok_bb), "plusi_first_%d", id);
  snprintf(loop_bb, sizeof(loop_bb), "plusi_loop_%d", id);
  snprintf(sep_ok_bb, sizeof(sep_ok_bb), "plusi_sep_%d", id);
  snprintf(body_bb, sizeof(body_bb), "plusi_body_%d", id);
  snprintf(end_bb, sizeof(end_bb), "plusi_end_%d", id);

  char acc_ptr[16];
  snprintf(acc_ptr, sizeof(acc_ptr), "%%r%d", irwriter_alloca(ctx->w, "i32"));
  irwriter_store(ctx->w, "i32", "0", acc_ptr);

  char first[16];
  _emit_leaf_call(ctx, unit, col_expr, first, sizeof(first));
  char cmp[16];
  snprintf(cmp, sizeof(cmp), "%%r%d", irwriter_icmp_imm(ctx->w, "slt", "i32", first, 0));
  irwriter_br_cond(ctx->w, cmp, on_fail, first_ok_bb);

  irwriter_bb(ctx->w, first_ok_bb);
  irwriter_store(ctx->w, "i32", first, acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  _gen_interlace_loop(ctx, unit, sep, col_expr, acc_ptr, loop_bb, sep_ok_bb, body_bb, end_bb, len_out, len_out_size);
}

// gen(e*<sep>, col, fail): (e (sep e)*)? — zero matches OK
static void _gen_star_interlace(GenCtx* ctx, PegUnit* unit, PegUnit* sep, const char* col_expr, char* len_out,
                                int32_t len_out_size) {
  int32_t id = ctx->label_counter++;
  char first_ok_bb[32], loop_bb[32], sep_ok_bb[32], body_bb[32], end_bb[32], empty_bb[32];
  snprintf(first_ok_bb, sizeof(first_ok_bb), "stari_first_%d", id);
  snprintf(loop_bb, sizeof(loop_bb), "stari_loop_%d", id);
  snprintf(sep_ok_bb, sizeof(sep_ok_bb), "stari_sep_%d", id);
  snprintf(body_bb, sizeof(body_bb), "stari_body_%d", id);
  snprintf(end_bb, sizeof(end_bb), "stari_end_%d", id);
  snprintf(empty_bb, sizeof(empty_bb), "stari_empty_%d", id);

  char acc_ptr[16];
  snprintf(acc_ptr, sizeof(acc_ptr), "%%r%d", irwriter_alloca(ctx->w, "i32"));
  irwriter_store(ctx->w, "i32", "0", acc_ptr);

  char first[16];
  _emit_leaf_call(ctx, unit, col_expr, first, sizeof(first));
  char first_neg[16];
  snprintf(first_neg, sizeof(first_neg), "%%r%d", irwriter_icmp_imm(ctx->w, "slt", "i32", first, 0));
  irwriter_br_cond(ctx->w, first_neg, empty_bb, first_ok_bb);

  irwriter_bb(ctx->w, first_ok_bb);
  irwriter_store(ctx->w, "i32", first, acc_ptr);
  irwriter_br(ctx->w, loop_bb);

  _gen_interlace_loop(ctx, unit, sep, col_expr, acc_ptr, loop_bb, sep_ok_bb, body_bb, end_bb, len_out, len_out_size);

  // Patch: emit empty_bb that jumps to end_bb. end_bb was already emitted by _gen_interlace_loop,
  // so we re-enter it via the phi/load at end_bb. Since acc_ptr is 0-initialized, loading it gives 0.
  irwriter_bb(ctx->w, empty_bb);
  irwriter_br(ctx->w, end_bb);
}

// --- Main gen() dispatcher ---

static void _gen_ir(GenCtx* ctx, PegUnit* unit, const char* col_expr, const char* on_fail, char* len_out,
                    int32_t len_out_size) {
  if (unit->kind == PEG_TOK || unit->kind == PEG_ID) {
    if (unit->multiplier == 0) {
      _gen_leaf(ctx, unit, col_expr, on_fail, len_out, len_out_size);
      return;
    }
    if (unit->multiplier == '?') {
      _gen_optional(ctx, unit, col_expr, len_out, len_out_size);
      return;
    }
    if (unit->multiplier == '+') {
      if (unit->interlace && unit->ninterlace > 0) {
        _gen_plus_interlace(ctx, unit, unit->interlace, col_expr, on_fail, len_out, len_out_size);
      } else {
        _gen_plus(ctx, unit, col_expr, on_fail, len_out, len_out_size);
      }
      return;
    }
    if (unit->multiplier == '*') {
      if (unit->interlace && unit->ninterlace > 0) {
        _gen_star_interlace(ctx, unit, unit->interlace, col_expr, len_out, len_out_size);
      } else {
        _gen_star(ctx, unit, col_expr, len_out, len_out_size);
      }
      return;
    }
  }

  if (unit->kind == PEG_SEQ) {
    int32_t n = (int32_t)darray_size(unit->children);
    if (n == 0) {
      _gen_empty(ctx, len_out, len_out_size);
      return;
    }

    // Sequence: gen(a b, col, fail) = r1 = gen(a); r2 = gen(b, col+r1); return r1+r2
    char total_ptr[16];
    snprintf(total_ptr, sizeof(total_ptr), "%%r%d", irwriter_alloca(ctx->w, "i32"));
    irwriter_store(ctx->w, "i32", "0", total_ptr);

    for (int32_t i = 0; i < n; i++) {
      char prev[16];
      snprintf(prev, sizeof(prev), "%%r%d", irwriter_load(ctx->w, "i32", total_ptr));
      char child_col[16];
      snprintf(child_col, sizeof(child_col), "%%r%d", irwriter_binop(ctx->w, "add", "i32", col_expr, prev));

      char child_len[32];
      _gen_ir(ctx, &unit->children[i], child_col, on_fail, child_len, sizeof(child_len));

      char new_total[16];
      snprintf(new_total, sizeof(new_total), "%%r%d", irwriter_load(ctx->w, "i32", total_ptr));
      char updated[16];
      snprintf(updated, sizeof(updated), "%%r%d", irwriter_binop(ctx->w, "add", "i32", new_total, child_len));
      irwriter_store(ctx->w, "i32", updated, total_ptr);
    }
    snprintf(len_out, (size_t)len_out_size, "%%r%d", irwriter_load(ctx->w, "i32", total_ptr));
    return;
  }

  if (unit->kind == PEG_BRANCHES) {
    int32_t n = (int32_t)darray_size(unit->children);
    if (n == 0) {
      _gen_empty(ctx, len_out, len_out_size);
      return;
    }

    // Ordered choice: save/restore/discard per spec
    int32_t choice_id = ctx->label_counter++;
    char done_bb[32];
    snprintf(done_bb, sizeof(done_bb), "choice_done_%d", choice_id);

    char result_ptr[16];
    snprintf(result_ptr, sizeof(result_ptr), "%%r%d", irwriter_alloca(ctx->w, "i32"));

    // save(col)
    peg_ir_backtrack_push(ctx->w, ctx->bt_stack, col_expr);

    for (int32_t i = 0; i < n; i++) {
      int32_t is_last = (i == n - 1);
      char alt_bb[32];
      if (!is_last) {
        snprintf(alt_bb, sizeof(alt_bb), "alt_%d_%d", choice_id, i + 1);
      }
      const char* fail_target = is_last ? on_fail : alt_bb;

      char r[32];
      _gen_ir(ctx, &unit->children[i], col_expr, fail_target, r, sizeof(r));

      // success: discard saved state
      peg_ir_backtrack_pop(ctx->w, ctx->bt_stack);
      irwriter_store(ctx->w, "i32", r, result_ptr);
      irwriter_br(ctx->w, done_bb);

      if (!is_last) {
        irwriter_bb(ctx->w, alt_bb);
        // restore + discard + re-save for next alternative
        char restored[16];
        snprintf(restored, sizeof(restored), "%%r%d", peg_ir_backtrack_restore(ctx->w, ctx->bt_stack));
        peg_ir_backtrack_pop(ctx->w, ctx->bt_stack);
        peg_ir_backtrack_push(ctx->w, ctx->bt_stack, restored);
      }
    }

    irwriter_bb(ctx->w, done_bb);
    snprintf(len_out, (size_t)len_out_size, "%%r%d", irwriter_load(ctx->w, "i32", result_ptr));
    return;
  }

  _gen_empty(ctx, len_out, len_out_size);
}

// --- Rule function generation ---

// Shared prologue: define function, entry BB, compute col_type_ref.
// Returns col_type_ref via out parameter. Caller emits cache-check logic,
// then calls _gen_rule_compute to set up backtrack stack, run _gen_ir, and get match_len.
static void _gen_rule_prologue(PegRule* rule, ScopeCtx* scope, IrWriter* w, char* col_type_ref,
                               int32_t col_type_ref_size) {
  const char* args[] = {"i8*", "i32"};
  const char* arg_names[] = {"table", "col"};

  char func_name[128];
  snprintf(func_name, sizeof(func_name), "parse_%s", rule->name);

  irwriter_define_start(w, func_name, "i32", 2, args, arg_names);
  irwriter_bb(w, "entry");

  snprintf(col_type_ref, (size_t)col_type_ref_size, "%%%s", scope->col_type);
}

// Shared compute phase: alloc backtrack stack, run _gen_ir, write match_len.
// Must be called after emitting "compute" BB.
static void _gen_rule_compute(PegRule* rule, IrWriter* w, char** tokens, const char* col_type_ref,
                              const char* fail_label, char* match_len, int32_t match_len_size) {
  char bt_stack[16];
  snprintf(bt_stack, sizeof(bt_stack), "%%r%d", irwriter_alloca(w, "%BtStack"));
  char bt_top_ptr[16];
  snprintf(bt_top_ptr, sizeof(bt_top_ptr), "%%r%d", irwriter_gep(w, "%BtStack", bt_stack, "i32 0, i32 1"));
  irwriter_store(w, "i32", "-1", bt_top_ptr);

  GenCtx ctx = {.w = w, .tokens = tokens, .col_type = col_type_ref, .bt_stack = bt_stack, .label_counter = 0};
  _gen_ir(&ctx, &rule->seq, "%col", fail_label, match_len, match_len_size);
}

static void _gen_rule_naive(PegRule* rule, RuleInfo* ri, ScopeCtx* scope, IrWriter* w, char** tokens) {
  char col_type_ref[72];
  _gen_rule_prologue(rule, scope, w, col_type_ref, sizeof(col_type_ref));

  char slot_val[16];
  snprintf(slot_val, sizeof(slot_val), "%%r%d", peg_ir_memo_get(w, col_type_ref, "%table", "%col", 0, ri->slot_idx));
  char cmp[16];
  snprintf(cmp, sizeof(cmp), "%%r%d", irwriter_icmp_imm(w, "ne", "i32", slot_val, -1));
  irwriter_br_cond(w, cmp, "cached", "compute");

  irwriter_bb(w, "cached");
  irwriter_ret(w, "i32", slot_val);

  irwriter_bb(w, "compute");
  char match_len[32];
  _gen_rule_compute(rule, w, tokens, col_type_ref, "fail", match_len, sizeof(match_len));

  peg_ir_memo_set(w, col_type_ref, "%table", "%col", 0, ri->slot_idx, match_len);
  irwriter_ret(w, "i32", match_len);

  irwriter_bb(w, "fail");
  peg_ir_memo_set(w, col_type_ref, "%table", "%col", 0, ri->slot_idx, "-1");
  irwriter_ret(w, "i32", "-1");

  irwriter_define_end(w);
}

static void _gen_rule_shared(PegRule* rule, RuleInfo* ri, ScopeCtx* scope, IrWriter* w, char** tokens) {
  char col_type_ref[72];
  _gen_rule_prologue(rule, scope, w, col_type_ref, sizeof(col_type_ref));

  char bit_ok[16];
  snprintf(bit_ok, sizeof(bit_ok), "%%r%d",
           peg_ir_bit_test(w, col_type_ref, "%table", "%col", ri->sg_id, ri->seg_mask));
  irwriter_br_cond(w, bit_ok, "check_slot", "bit_fail");

  irwriter_bb(w, "check_slot");
  char slot_val[16];
  snprintf(slot_val, sizeof(slot_val), "%%r%d", peg_ir_memo_get(w, col_type_ref, "%table", "%col", 1, ri->slot_idx));
  char slot_cached[16];
  snprintf(slot_cached, sizeof(slot_cached), "%%r%d", irwriter_icmp_imm(w, "ne", "i32", slot_val, -1));
  irwriter_br_cond(w, slot_cached, "cached", "compute");

  irwriter_bb(w, "cached");
  irwriter_ret(w, "i32", slot_val);

  irwriter_bb(w, "compute");
  char match_len[32];
  _gen_rule_compute(rule, w, tokens, col_type_ref, "match_fail", match_len, sizeof(match_len));

  peg_ir_bit_exclude(w, col_type_ref, "%table", "%col", ri->sg_id, ri->seg_mask);
  peg_ir_memo_set(w, col_type_ref, "%table", "%col", 1, ri->slot_idx, match_len);
  irwriter_ret(w, "i32", match_len);

  irwriter_bb(w, "match_fail");
  peg_ir_bit_deny(w, col_type_ref, "%table", "%col", ri->sg_id, ri->seg_mask);
  irwriter_ret(w, "i32", "-1");

  irwriter_bb(w, "bit_fail");
  irwriter_ret(w, "i32", "-1");

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
