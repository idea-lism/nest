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
  _walk_unit(input, &rule->seq, defined, visited);
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
    _walk_unit(input, &pr->seq, &sc.defined_rules, &vis);
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
      int32_t bid = _breakdown_unit(input, sc, branch, n, i);
      // branch calling branch: wrap in seq (by AST kind for now; fixup pass handles lowered kind)
      if (branch->kind == PEG_BRANCHES) {
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
        char wn_buf[256];
        snprintf(wn_buf, sizeof(wn_buf), "%s$%d", r->name, j + 1000);
        char* wn = strdup(wn_buf);
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
        root_id = _breakdown_unit(input, sc, &pr->seq, rn, -1);
      }
      darray_push(sc->root_ids, root_id);
    }

    _fixup_calls(sc);
  }
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
  for (int32_t i = 0; i < n; i++) {
    sc->rules[i].slot_index = (uint32_t)i;
    sc->rules[i].segment_index = 0;
    sc->rules[i].segment_mask = 0;
    sc->rules[i].rule_bit_mask = 0;
  }
}

static void _assign_colored(ScopeClosure* sc) {
  int32_t n = (int32_t)darray_size(sc->rules);
  if (n <= 1) {
    _assign_naive(sc);
    return;
  }

  Graph* g = graph_new(n);
  for (int32_t i = 0; i < n; i++) {
    for (int32_t j = i + 1; j < n; j++) {
      if (!_exclusive(&sc->rules[i], &sc->rules[j])) {
        graph_add_edge(g, i, j);
      }
    }
  }

  ColoringResult* cr = NULL;
  int32_t k;
  for (k = 1; k <= n; k++) {
    cr = coloring_solve(n, graph_edges(g), graph_n_edges(g), k, 50000, 42);
    if (cr) {
      break;
    }
  }
  graph_del(g);

  if (!cr) {
    _assign_naive(sc);
    return;
  }

  int32_t sg_size = coloring_get_sg_size(cr);
  int32_t* sg_slot = malloc(sg_size * sizeof(int32_t));
  memset(sg_slot, -1, sg_size * sizeof(int32_t));
  int32_t next_slot = 0;

  for (int32_t i = 0; i < n; i++) {
    int32_t sg_id, seg_mask;
    coloring_get_segment_info(cr, i, &sg_id, &seg_mask);
    sc->rules[i].segment_index = (uint32_t)sg_id;
    sc->rules[i].rule_bit_mask = (uint32_t)seg_mask;
    sc->rules[i].segment_mask = (uint32_t)seg_mask;
    if (sg_slot[sg_id] < 0) {
      sg_slot[sg_id] = next_slot++;
    }
    sc->rules[i].slot_index = (uint32_t)sg_slot[sg_id];
  }

  free(sg_slot);
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
      if ((int32_t)sc->rules[i].segment_index + 1 > n_sg) {
        n_sg = (int32_t)sc->rules[i].segment_index + 1;
      }
      if ((int32_t)sc->rules[i].slot_index + 1 > n_slots) {
        n_slots = (int32_t)sc->rules[i].slot_index + 1;
      }
    }
  } else {
    n_slots = n_rules;
  }

  int32_t col_sizeof = n_sg * 4 + n_slots * 4;
  if (col_sizeof == 0) {
    col_sizeof = 4;
  }

  const char* at[] = {"ptr", "ptr"};
  const char* an[] = {"tt", "stack_mem"};
  char fn[256];
  snprintf(fn, sizeof(fn), "parse_%s", sc->scope_name);
  irwriter_define_start(w, fn, "void", 2, at, an);

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
  IrVal n_tokens = irwriter_load(w, "i32", cnt_pp);

  IrVal ntok64 = irwriter_sext(w, "i32", n_tokens, "i64");
  IrVal csz64 = irwriter_sext(w, "i32", irwriter_imm_int(w, col_sizeof), "i64");
  IrVal tbl_bytes = irwriter_binop(w, "mul", "i64", ntok64, csz64);
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

  IrVal col_a = irwriter_alloca(w, "i32");
  irwriter_store(w, "i32", irwriter_imm_int(w, 0), col_a);
  IrVal sp_a = irwriter_alloca(w, "ptr");
  irwriter_store(w, "ptr", irwriter_imm(w, "%stack_mem"), sp_a);
  IrVal bp_a = irwriter_alloca(w, "ptr");
  irwriter_store(w, "ptr", irwriter_imm(w, "%stack_mem"), bp_a);
  IrVal ret_a = irwriter_alloca(w, "i32");

  PegIrCtx ctx = {
      .w = w,
      .tokens = tokens,
      .col_index = col_a,
      .stack = sp_a,
      .stack_bp = bp_a,
      .ret_val = ret_a,
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

  int32_t fail_bb = irwriter_label(w);
  int32_t succ_bb = irwriter_label(w);
  ctx.fail_label = fail_bb;

  // call entry rule inline
  IrVal result = peg_ir_element(&ctx, sc->rules[entry_id].kind, entry_id);
  (void)result;
  irwriter_br(w, succ_bb);

  // Emit per-rule labeled blocks for memoized parsing
  for (int32_t i = 0; i < n_rules; i++) {
    ScopedRule* rule = &sc->rules[i];

    irwriter_rawf(w, "%s:\n", rule->name);

    int32_t rule_done_bb = irwriter_label(w);
    int32_t rule_compute_bb = irwriter_label(w);
    int32_t rule_miss_bb = irwriter_label(w);

    IrVal col = irwriter_load(w, "i32", col_a);

    if (compress && n_sg > 0) {
      // row_shared: bit test first
      IrVal bit_ok = peg_ir_bit_test(&ctx, col, rule->segment_index, rule->rule_bit_mask);
      int32_t bit_pass_bb = irwriter_label(w);
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
    int32_t parse_fail_bb = irwriter_label(w);
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
    PegUnit* seq = &rule->seq;

    bool has_branches = false;
    int32_t nc = seq->children ? (int32_t)darray_size(seq->children) : 0;
    for (int32_t i = 0; i < nc; i++) {
      if (seq->children[i].kind == PEG_BRANCHES) {
        has_branches = true;
      }
    }

    char nn[256];
    snprintf(nn, sizeof(nn), "%s_%s_Node", prefix, rn);

    hw_fmt(hw, "typedef struct {\n");

    if (has_branches) {
      hw_raw(hw, "  struct {\n");
      for (int32_t i = 0; i < nc; i++) {
        if (seq->children[i].kind != PEG_BRANCHES) {
          continue;
        }
        PegUnit* br = &seq->children[i];
        for (int32_t j = 0; j < (int32_t)darray_size(br->children); j++) {
          const char* tag = br->children[j].tag;
          if (tag && tag[0]) {
            hw_fmt(hw, "    bool %s : 1;\n", tag);
          }
        }
      }
      hw_raw(hw, "  } is;\n");
    }

    for (int32_t i = 0; i < nc; i++) {
      PegUnit* child = &seq->children[i];
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
        // For a branch rule, val is the chosen branch's scoped_rule_id
        // Set the is.* bitfield based on which branch was chosen
        int32_t branch_idx = 0;
        for (int32_t i = 0; i < nc; i++) {
          if (seq->children[i].kind != PEG_BRANCHES) {
            continue;
          }
          PegUnit* br = &seq->children[i];
          for (int32_t j = 0; j < (int32_t)darray_size(br->children); j++) {
            const char* tag = br->children[j].tag;
            if (tag && tag[0]) {
              hw_fmt(hw, "  node.is.%s = (val == %d);\n", tag, branch_idx);
            }
            branch_idx++;
          }
        }
      }
      hw_raw(hw, "  (void)val;\n");
    }

    hw_fmt(hw, "  return node;\n");
    hw_fmt(hw, "}\n\n");
  }

  // scope size defines
  for (int32_t ci = 0; ci < n_closures; ci++) {
    ScopeClosure* sc = &closures[ci];
    int32_t nr = (int32_t)darray_size(sc->rules);
    char dn[256];
    snprintf(dn, sizeof(dn), "%s_%s_N_RULES", prefix, sc->scope_name);
    for (char* p = dn; *p; p++) {
      if (*p >= 'a' && *p <= 'z') {
        *p = (char)(*p - 32);
      }
    }
    hw_define(hw, dn, nr);
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

  for (int32_t i = 0; i < n_closures; i++) {
    _compute_nullable(&closures[i]);
    _compute_first_last(&closures[i]);
    if (compress_memoize) {
      _assign_colored(&closures[i]);
    } else {
      _assign_naive(&closures[i]);
    }
  }

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
