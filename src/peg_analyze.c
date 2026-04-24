// specs/peg_analyze.md
#include "bitset.h"
#include "darray.h"
#include "peg.h"
#include "symtab.h"
#include "xmalloc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ============================================================
// Helpers
// ============================================================

__attribute__((format(printf, 1, 2))) static char* _fmt(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char* buf = XMALLOC((size_t)n + 1);
  va_start(ap, fmt);
  vsnprintf(buf, (size_t)n + 1, fmt, ap);
  va_end(ap);
  return buf;
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
  lu.rule_by_gid = XCALLOC((size_t)lu.rule_count, sizeof(PegRule*));
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

static void _free_rule_lookup(RuleLookup* lu) { XFREE(lu->rule_by_gid); }

static bool _is_scope_term(PegAnalyzeInput* input, int32_t id) {
  return id < (int32_t)symtab_count(&input->scope_names) + input->scope_names.start_num;
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
      int32_t saved_mult_num = gctx->multiplier_num;
      gctx->multiplier_num = 0;
      ScopedUnit body = _breakdown(gctx, &callee->body, callee_name);
      gctx->closure->scoped_rules[idx].body = body;
      gctx->multiplier_num = saved_mult_num;
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

  // Handle lookahead predicates: & and ! create SCOPED_UNIT_AND / SCOPED_UNIT_NOT
  // Lookaheads are inline-only, no child memoize rules generated.
  if (unit->lookahead) {
    PegUnit inner_copy = *unit;
    inner_copy.lookahead = 0;
    ScopedUnit inner = _breakdown(gctx, &inner_copy, parent_rule_name);
    ScopedUnit* inner_heap = XMALLOC(sizeof(ScopedUnit));
    *inner_heap = inner;
    su.kind = (unit->lookahead == '&') ? SCOPED_UNIT_AND : SCOPED_UNIT_NOT;
    su.as.base = inner_heap;
    return su;
  }

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
    ScopedUnit* lhs_heap = XMALLOC(sizeof(ScopedUnit));
    *lhs_heap = lhs;

    ScopedUnit* rhs_heap = NULL;
    if (unit->interlace_rhs_kind) {
      PegUnit rhs_unit = {.kind = unit->interlace_rhs_kind, .id = unit->interlace_rhs_id};
      ScopedUnit rhs = _breakdown_single(gctx, &rhs_unit);
      rhs_heap = XMALLOC(sizeof(ScopedUnit));
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
  case SCOPED_UNIT_AND:
  case SCOPED_UNIT_NOT:
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

// Force tag_bit_local_offset = -1 on all units inside a lookahead subtree.
static void _force_no_tags(ScopedUnit* su) {
  su->tag_bit_local_offset = -1;
  switch (su->kind) {
  case SCOPED_UNIT_SEQ:
  case SCOPED_UNIT_BRANCHES:
    for (size_t i = 0; i < darray_size(su->as.children); i++) {
      _force_no_tags(&su->as.children[i]);
    }
    break;
  case SCOPED_UNIT_MAYBE:
  case SCOPED_UNIT_AND:
  case SCOPED_UNIT_NOT:
    _force_no_tags(su->as.base);
    break;
  case SCOPED_UNIT_STAR:
  case SCOPED_UNIT_PLUS:
    _force_no_tags(su->as.interlace.lhs);
    if (su->as.interlace.rhs) {
      _force_no_tags(su->as.interlace.rhs);
    }
    break;
  default:
    break;
  }
}

// Walk the tree and force -1 tags on lookahead inner subtrees.
static void _strip_lookahead_tags(ScopedUnit* su) {
  switch (su->kind) {
  case SCOPED_UNIT_AND:
  case SCOPED_UNIT_NOT:
    su->tag_bit_local_offset = -1;
    _force_no_tags(su->as.base);
    break;
  case SCOPED_UNIT_SEQ:
  case SCOPED_UNIT_BRANCHES:
    for (size_t i = 0; i < darray_size(su->as.children); i++) {
      _strip_lookahead_tags(&su->as.children[i]);
    }
    break;
  case SCOPED_UNIT_MAYBE:
    _strip_lookahead_tags(su->as.base);
    break;
  case SCOPED_UNIT_STAR:
  case SCOPED_UNIT_PLUS:
    _strip_lookahead_tags(su->as.interlace.lhs);
    if (su->as.interlace.rhs) {
      _strip_lookahead_tags(su->as.interlace.rhs);
    }
    break;
  default:
    break;
  }
}

// ============================================================
// Node field layout computation
// ============================================================

static char* _sanitize_field_name(const char* name) {
  size_t len = strlen(name);
  char* out = XMALLOC(len + 2);
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
    XFREE(fd->names[i]);
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

// find entry rule row in target scope (entry rule name = scope name)
static int32_t _scope_entry_row(PegAnalyzeInput* input, ScopeClosure* all_closures, int32_t n_closures,
                                int32_t scope_id) {
  const char* scope_name = symtab_get(&input->scope_names, scope_id);
  for (int32_t c = 0; c < n_closures; c++) {
    ScopeClosure* tcl = &all_closures[c];
    if (strcmp(tcl->scope_name, scope_name) != 0) {
      continue;
    }
    int32_t entry_id = symtab_find_f(&tcl->scoped_rule_names, "%s$%s", scope_name, scope_name);
    ScopedRule* entry_sr = _find_scoped_rule(tcl, entry_id);
    return entry_sr ? _slot_row(tcl, entry_sr) : 0;
  }
  return 0;
}

// term: sanitize name, dedup, set advance based on is_link/in_branch context
static NodeField _build_term_node_field(PegAnalyzeInput* input, ScopeClosure* cl, ScopeClosure* all_closures,
                                        int32_t n_closures, FieldDedup* fd, int32_t term_id, bool is_link,
                                        bool in_branch, const char* parent_rule_name, int32_t* mult_num,
                                        int32_t branch_index) {
  char* san = _sanitize_field_name(_term_name(input, term_id));
  char* dedup = _field_dedup_next(fd, san);
  bool scope = _is_scope_term(input, term_id);
  NodeField nf = {
      .name = dedup,
      .is_link = is_link,
      .is_scope = scope,
      .ref_row = scope ? _scope_entry_row(input, all_closures, n_closures, term_id) : 0,
      .rhs_row = PEG_ROW_NONE,
      .advance = (is_link || in_branch) ? NODE_ADVANCE_NONE : NODE_ADVANCE_ONE,
      .advance_slot_row = 0,
      .wrapper_name = NULL,
      .branch_index = branch_index,
  };
  if (is_link) {
    (*mult_num)++;
    const char* wrapper_name =
        symtab_get(&cl->scoped_rule_names,
                   symtab_find_f(&cl->scoped_rule_names, "%s$%s$%d", cl->scope_name, parent_rule_name, *mult_num));
    ScopedRule* wrapper =
        wrapper_name ? _find_scoped_rule(cl, symtab_find(&cl->scoped_rule_names, wrapper_name)) : NULL;
    nf.ref_row = wrapper ? _slot_row(cl, wrapper) : 0;
    nf.wrapper_name = wrapper_name;
  }
  XFREE(san);
  return nf;
}

// call: dedup name, resolve callee/wrapper row, interlace rhs if applicable
static NodeField _build_call_node_field(PegAnalyzeInput* input, ScopeClosure* cl, FieldDedup* fd, int32_t call_id,
                                        bool is_link, bool in_branch, PegUnit* u, const char* parent_rule_name,
                                        int32_t* mult_num, int32_t branch_index) {
  const char* callee_name = symtab_get(&input->rule_names, call_id);
  char* dedup = _field_dedup_next(fd, callee_name);
  NodeField nf = {
      .name = dedup,
      .is_link = is_link,
      .is_scope = false,
      .ref_row = 0,
      .rhs_row = PEG_ROW_NONE,
      .advance = NODE_ADVANCE_NONE,
      .advance_slot_row = 0,
      .wrapper_name = NULL,
      .branch_index = branch_index,
  };
  if (is_link) {
    (*mult_num)++;
    const char* wrapper_name =
        symtab_get(&cl->scoped_rule_names,
                   symtab_find_f(&cl->scoped_rule_names, "%s$%s$%d", cl->scope_name, parent_rule_name, *mult_num));
    ScopedRule* wrapper =
        wrapper_name ? _find_scoped_rule(cl, symtab_find(&cl->scoped_rule_names, wrapper_name)) : NULL;
    nf.ref_row = wrapper ? _slot_row(cl, wrapper) : 0;
    nf.wrapper_name = wrapper_name;
    if (u && u->interlace_rhs_kind == PEG_CALL) {
      const char* rhs_name = symtab_get(&input->rule_names, u->interlace_rhs_id);
      ScopedRule* rhs_sr =
          _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s", cl->scope_name, rhs_name));
      if (rhs_sr) {
        nf.rhs_row = _slot_row(cl, rhs_sr);
      }
    } else if (u && u->interlace_rhs_kind == PEG_TERM) {
      nf.rhs_row = PEG_ROW_TERM;
    }
  } else {
    ScopedRule* callee_sr =
        _find_scoped_rule(cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s", cl->scope_name, callee_name));
    int32_t callee_row = callee_sr ? _slot_row(cl, callee_sr) : 0;
    nf.ref_row = callee_row;
    if (!in_branch) {
      nf.advance = NODE_ADVANCE_SLOT;
      nf.advance_slot_row = callee_row;
    }
  }
  return nf;
}
static void _build_child_fields(PegAnalyzeInput* input, ScopeClosure* cl, ScopeClosure* all_closures,
                                int32_t n_closures, PegUnit* children, int32_t child_size, FieldDedup* fd,
                                const char* parent_rule_name, int32_t* multiplier_num_ptr, NodeFields* out,
                                int32_t branch_index) {
  for (int32_t i = 0; i < child_size; i++) {
    PegUnit* u = &children[i];
    // Lookahead predicates consume no input and produce no node fields
    if (u->lookahead) {
      continue;
    }
    bool is_link = (u->multiplier == '*' || u->multiplier == '+');
    if (u->kind == PEG_TERM) {
      darray_push(*out, _build_term_node_field(input, cl, all_closures, n_closures, fd, u->id, is_link, false,
                                               parent_rule_name, multiplier_num_ptr, branch_index));
      // link field followed by more children: advance cursor past repetition via wrapper slot
      if (is_link && i < child_size - 1) {
        NodeField* last = &(*out)[darray_size(*out) - 1];
        last->advance = NODE_ADVANCE_SLOT;
        last->advance_slot_row = last->ref_row;
      }
    } else if (u->kind == PEG_CALL) {
      darray_push(*out, _build_call_node_field(input, cl, fd, u->id, is_link, false, u, parent_rule_name,
                                               multiplier_num_ptr, branch_index));
      // link field followed by more children: advance cursor past repetition via wrapper slot
      if (is_link && i < child_size - 1) {
        NodeField* last = &(*out)[darray_size(*out) - 1];
        last->advance = NODE_ADVANCE_SLOT;
        last->advance_slot_row = last->ref_row;
      }
    } else if (u->kind == PEG_BRANCHES) {
      for (size_t j = 0; j < darray_size(u->children); j++) {
        PegUnit* branch = &u->children[j];
        if (branch->kind == PEG_TERM) {
          darray_push(*out, _build_term_node_field(input, cl, all_closures, n_closures, fd, branch->id, false, true,
                                                   parent_rule_name, multiplier_num_ptr, (int32_t)j));
        } else if (branch->kind == PEG_CALL) {
          darray_push(*out, _build_call_node_field(input, cl, fd, branch->id, false, true, NULL, parent_rule_name,
                                                   multiplier_num_ptr, (int32_t)j));
        } else if (branch->kind == PEG_SEQ) {
          for (size_t k = 0; k < darray_size(branch->children); k++) {
            PegUnit* bc = &branch->children[k];
            if (bc->kind == PEG_TERM) {
              darray_push(*out, _build_term_node_field(input, cl, all_closures, n_closures, fd, bc->id, false, true,
                                                       parent_rule_name, multiplier_num_ptr, (int32_t)j));
            } else if (bc->kind == PEG_CALL) {
              darray_push(*out, _build_call_node_field(input, cl, fd, bc->id, false, true, NULL, parent_rule_name,
                                                       multiplier_num_ptr, (int32_t)j));
            }
          }
        }
      }
      // advance past the branch via wrapper rule's slot
      (*multiplier_num_ptr)++;
      ScopedRule* wrapper = _find_scoped_rule(
          cl, symtab_find_f(&cl->scoped_rule_names, "%s$%s$%d", cl->scope_name, parent_rule_name, *multiplier_num_ptr));
      if (wrapper && darray_size(*out) > 0) {
        NodeField* last = &(*out)[darray_size(*out) - 1];
        if (last->advance == NODE_ADVANCE_NONE) {
          last->advance = NODE_ADVANCE_SLOT;
          last->advance_slot_row = _slot_row(cl, wrapper);
        }
      }
    }
  }
}
static void _build_node_fields(PegAnalyzeInput* input, RuleLookup* lu, ScopeClosure* cl, ScopeClosure* all_closures,
                               int32_t n_closures) {
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

    const char* orig_name = symtab_get(&input->rule_names, sr->original_global_id);
    sr->node_fields = darray_new(sizeof(NodeField), 0);
    FieldDedup fd;
    _field_dedup_init(&fd);
    int32_t multiplier_num = 0;
    if (orig->body.kind == PEG_SEQ) {
      _build_child_fields(input, cl, all_closures, n_closures, orig->body.children,
                          (int32_t)darray_size(orig->body.children), &fd, orig_name, &multiplier_num, &sr->node_fields,
                          -1);
    } else if (orig->body.kind == PEG_BRANCHES) {
      for (size_t j = 0; j < darray_size(orig->body.children); j++) {
        PegUnit* branch = &orig->body.children[j];
        multiplier_num = 0;
        if (branch->kind == PEG_SEQ) {
          _build_child_fields(input, cl, all_closures, n_closures, branch->children,
                              (int32_t)darray_size(branch->children), &fd, orig_name, &multiplier_num, &sr->node_fields,
                              (int32_t)j);
        } else if (branch->kind == PEG_TERM) {
          darray_push(sr->node_fields, _build_term_node_field(input, cl, all_closures, n_closures, &fd, branch->id,
                                                              false, true, orig_name, &multiplier_num, (int32_t)j));
        } else if (branch->kind == PEG_CALL) {
          darray_push(sr->node_fields, _build_call_node_field(input, cl, &fd, branch->id, false, true, NULL, orig_name,
                                                              &multiplier_num, (int32_t)j));
        }
      }
    } else if (orig->body.kind == PEG_TERM) {
      bool is_link = (orig->body.multiplier == '*' || orig->body.multiplier == '+');
      darray_push(sr->node_fields, _build_term_node_field(input, cl, all_closures, n_closures, &fd, orig->body.id,
                                                          is_link, false, orig_name, &multiplier_num, -1));
    } else if (orig->body.kind == PEG_CALL) {
      bool is_link = (orig->body.multiplier == '*' || orig->body.multiplier == '+');
      darray_push(sr->node_fields, _build_call_node_field(input, cl, &fd, orig->body.id, is_link, false, &orig->body,
                                                          orig_name, &multiplier_num, -1));
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
  case SCOPED_UNIT_AND:
  case SCOPED_UNIT_NOT:
    if (su->as.base) {
      _free_scoped_unit(su->as.base);
      XFREE(su->as.base);
    }
    break;
  case SCOPED_UNIT_STAR:
  case SCOPED_UNIT_PLUS:
    if (su->as.interlace.lhs) {
      _free_scoped_unit(su->as.interlace.lhs);
      XFREE(su->as.interlace.lhs);
    }
    if (su->as.interlace.rhs) {
      _free_scoped_unit(su->as.interlace.rhs);
      XFREE(su->as.interlace.rhs);
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
    XFREE(fields[i].name);
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
    if (cl->scoped_rules[i].call_sites) {
      darray_del(cl->scoped_rules[i].call_sites);
    }
  }
  darray_del(cl->scoped_rules);
  symtab_free(&cl->scoped_rule_names);
}

// ============================================================
// Call site analysis
// ============================================================

static void _walk_call_sites(ScopeClosure* cl, int32_t caller_id, int32_t* site_counter, ScopedUnit* su) {
  switch (su->kind) {
  case SCOPED_UNIT_CALL: {
    int32_t cid = symtab_find(&cl->scoped_rule_names, su->as.callee);
    if (cid >= 0) {
      int32_t callee_idx = cid - cl->scoped_rule_names.start_num;
      CallSite cs = {.caller_id = caller_id, .site = (*site_counter)++};
      darray_push(cl->scoped_rules[callee_idx].call_sites, cs);
    }
    break;
  }
  case SCOPED_UNIT_SEQ:
  case SCOPED_UNIT_BRANCHES:
    for (size_t i = 0; i < darray_size(su->as.children); i++) {
      _walk_call_sites(cl, caller_id, site_counter, &su->as.children[i]);
    }
    break;
  case SCOPED_UNIT_MAYBE:
  case SCOPED_UNIT_AND:
  case SCOPED_UNIT_NOT:
    _walk_call_sites(cl, caller_id, site_counter, su->as.base);
    break;
  case SCOPED_UNIT_STAR:
    // star without interlace: lhs called once (loop)
    // star with interlace: lhs called twice (first elem + loop), rhs once (loop)
    _walk_call_sites(cl, caller_id, site_counter, su->as.interlace.lhs);
    if (su->as.interlace.rhs) {
      _walk_call_sites(cl, caller_id, site_counter, su->as.interlace.lhs);
      _walk_call_sites(cl, caller_id, site_counter, su->as.interlace.rhs);
    }
    break;
  case SCOPED_UNIT_PLUS:
    // plus: lhs called twice (first elem + loop), rhs once if interlaced
    _walk_call_sites(cl, caller_id, site_counter, su->as.interlace.lhs);
    _walk_call_sites(cl, caller_id, site_counter, su->as.interlace.lhs);
    if (su->as.interlace.rhs) {
      _walk_call_sites(cl, caller_id, site_counter, su->as.interlace.rhs);
    }
    break;
  case SCOPED_UNIT_TERM:
    break;
  }
}

static void _compute_call_sites(ScopeClosure* cl) {
  int32_t rule_size = (int32_t)darray_size(cl->scoped_rules);
  for (int32_t i = 0; i < rule_size; i++) {
    cl->scoped_rules[i].call_sites = darray_new(sizeof(CallSite), 0);
  }

  // entrance call: scope calls scoped_rules[0]
  CallSite entry = {.caller_id = -1, .site = 0};
  darray_push(cl->scoped_rules[0].call_sites, entry);

  for (int32_t i = 0; i < rule_size; i++) {
    int32_t site_counter = 0;
    _walk_call_sites(cl, i, &site_counter, &cl->scoped_rules[i].body);
  }
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
      .verbose = input->verbose_level,
  };

  int32_t rule_size = (int32_t)darray_size(input->rules);
  if (rule_size == 0) {
    result.scope_closures = darray_new(sizeof(ScopeClosure), 0);
    return result;
  }

  RuleLookup lu = _build_rule_lookup(input);

  if (input->verbose_level) {
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

  // Force tag_bit_local_offset = -1 inside lookahead subtrees
  for (int32_t c = 0; c < closure_size; c++) {
    for (size_t i = 0; i < darray_size(closures[c].scoped_rules); i++) {
      _strip_lookahead_tags(&closures[c].scoped_rules[i].body);
    }
  }

  if (input->verbose_level > 1) {
    fprintf(stderr, "  [peg] %d scope closures, memoize=%s\n", closure_size,
            memoize_mode == MEMOIZE_SHARED  ? "shared"
            : memoize_mode == MEMOIZE_NAIVE ? "naive"
                                            : "none");
  }

  for (int32_t c = 0; c < closure_size; c++) {
    if (input->verbose_level) {
      fprintf(stderr, "  [peg] analyzing scope '%s'\n", closures[c].scope_name);
    }
    peg_alloc_scope(&closures[c], (MemoizeMode)memoize_mode);
    if (input->verbose_level > 1) {
      ScopeClosure* cl = &closures[c];
      int32_t rule_size = (int32_t)darray_size(cl->scoped_rules);
      int64_t aligned_slots = (cl->slots_size + 1) & ~(int64_t)1;
      int64_t sizeof_col = cl->bits_bucket_size * 8 + aligned_slots * 4;
      fprintf(stderr, "  [peg]   %d rules, %d color groups, sizeof_col=%lld bytes\n", rule_size, (int)cl->slots_size,
              (long long)sizeof_col);
      fprintf(stderr, "  [peg]   column layout: [bits %lld x i64][slots %lld x i32]\n", (long long)cl->bits_bucket_size,
              (long long)aligned_slots);
      for (int32_t r = 0; r < rule_size; r++) {
        ScopedRule* sr = &cl->scoped_rules[r];
        int32_t tag_size = symtab_count(&sr->tags);
        fprintf(stderr, "  [peg]     %s: slot[%llu]", sr->scoped_rule_name, (unsigned long long)sr->slot_index);
        if (memoize_mode == MEMOIZE_SHARED) {
          fprintf(stderr, " seg[%llu]=0x%llx", (unsigned long long)sr->segment_index,
                  (unsigned long long)sr->rule_bit_mask);
        }
        if (tag_size > 0) {
          fprintf(stderr, " %d tags @bucket[%d]+%d", tag_size, sr->tag_bit_index, sr->tag_bit_offset);
        }
        fprintf(stderr, "\n");
      }
    }
  }

  for (int32_t c = 0; c < closure_size; c++) {
    _build_node_fields(input, &lu, &closures[c], closures, closure_size);
  }

  for (int32_t c = 0; c < closure_size; c++) {
    _compute_call_sites(&closures[c]);
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
