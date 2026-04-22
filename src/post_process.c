// specs/post_process.md
#include "post_process.h"
#include "darray.h"
#include "peg.h"
#include "symtab.h"
#include "vpa.h"
#include "xmalloc.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// --- VpaUnit helpers ---

static void _free_vpa_unit(VpaUnit* u) {
  re_ir_free(u->re);
  darray_del(u->action_units);
  XFREE(u->macro_name);
}

static VpaUnit _clone_vpa_unit(VpaUnit* src) {
  VpaUnit dst = *src;
  dst.re = re_ir_clone(src->re);
  dst.macro_name = src->macro_name ? strdup(src->macro_name) : NULL;
  if (src->action_units) {
    int32_t n = (int32_t)darray_size(src->action_units);
    dst.action_units = darray_new(sizeof(int32_t), (size_t)n);
    for (int32_t i = 0; i < n; i++) {
      dst.action_units[i] = src->action_units[i];
    }
  }
  return dst;
}

// --- Helper functions ---

static bool _has_leader(VpaScope* s) { return s->leader.re != NULL; }

// --- Integer set utilities (darray of int32_t) ---

static bool _id_set_has(int32_t* set, int32_t id) {
  for (int32_t i = 0; i < (int32_t)darray_size(set); i++) {
    if (set[i] == id) {
      return true;
    }
  }
  return false;
}

static void _id_set_add(int32_t** set, int32_t id) {
  if (!_id_set_has(*set, id)) {
    darray_push(*set, id);
  }
}

// --- Lookup functions ---

static VpaScope* _find_scope(ParseState* ps, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_scopes); i++) {
    if (strcmp(ps->vpa_scopes[i].name, name) == 0) {
      return &ps->vpa_scopes[i];
    }
  }
  return NULL;
}

static const char* _rule_name(ParseState* ps, int32_t global_id) { return symtab_get(&ps->rule_names, global_id); }

static PegRule* _find_peg_rule(ParseState* ps, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(ps->peg_rules); i++) {
    const char* rn = _rule_name(ps, ps->peg_rules[i].global_id);
    if (rn && strcmp(rn, name) == 0) {
      return &ps->peg_rules[i];
    }
  }
  return NULL;
}

// --- Post-processing: inline macros ---

static VpaScope* _find_macro(ParseState* ps, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_scopes); i++) {
    if (ps->vpa_scopes[i].is_macro && strcmp(ps->vpa_scopes[i].name, name) == 0) {
      return &ps->vpa_scopes[i];
    }
  }
  return NULL;
}

bool pp_inline_macros(ParseState* ps) {
  // inline macro references (VPA_MACRO_REF)
  for (int32_t r = 0; r < (int32_t)darray_size(ps->vpa_scopes); r++) {
    VpaScope* scope = &ps->vpa_scopes[r];
    if (scope->is_macro) {
      continue;
    }
    for (int32_t i = 0; i < (int32_t)darray_size(scope->children); i++) {
      VpaUnit* unit = &scope->children[i];
      if (unit->kind != VPA_MACRO_REF) {
        continue;
      }
      VpaScope* macro = _find_macro(ps, unit->macro_name);
      if (!macro) {
        parse_error(ps, "macro '%s' not found (referenced in scope '%s')", unit->macro_name, scope->name);
        return false;
      }

      int32_t macro_n = (int32_t)darray_size(macro->children);
      int32_t new_count = (int32_t)darray_size(scope->children) - 1 + macro_n;
      int32_t tail = (int32_t)darray_size(scope->children) - i - 1;

      _free_vpa_unit(unit);

      scope->children = darray_grow(scope->children, (size_t)new_count);

      if (tail > 0 && macro_n != 1) {
        memmove(&scope->children[i + macro_n], &scope->children[i + 1], (size_t)tail * sizeof(VpaUnit));
      }

      for (int32_t m = 0; m < macro_n; m++) {
        scope->children[i + m] = _clone_vpa_unit(&macro->children[m]);
      }
      i += macro_n - 1;
    }
  }

  // Check sub-scope calls: if a sub-scope call targets a macro, report error
  for (int32_t r = 0; r < (int32_t)darray_size(ps->vpa_scopes); r++) {
    VpaScope* scope = &ps->vpa_scopes[r];
    if (scope->is_macro) {
      continue;
    }
    for (int32_t i = 0; i < (int32_t)darray_size(scope->children); i++) {
      VpaUnit* unit = &scope->children[i];
      if (unit->kind != VPA_CALL) {
        continue;
      }
      bool is_real_scope = false;
      for (int32_t j = 0; j < (int32_t)darray_size(ps->vpa_scopes); j++) {
        if (ps->vpa_scopes[j].scope_id == unit->call_scope_id && !ps->vpa_scopes[j].is_macro) {
          is_real_scope = true;
          break;
        }
      }
      if (!is_real_scope) {
        const char* callee_name = symtab_get(&ps->scope_names, unit->call_scope_id);
        char* macro_guess = parse_sfmt("*%s", callee_name);
        VpaScope* maybe_macro = _find_macro(ps, macro_guess);
        XFREE(macro_guess);
        if (maybe_macro) {
          parse_error(ps, "scope '%s': '%s' is a macro, use '*%s' to reference it", scope->name, callee_name,
                      callee_name);
        } else {
          parse_error(ps, "scope '%s': sub-scope '%s' is not defined", scope->name, callee_name);
        }
        return false;
      }
    }
  }

  // --- Compact: remove macros and non-scope rules, scope_ids are already assigned ---
  VpaScope* old = ps->vpa_scopes;
  VpaScope* fresh = darray_new(sizeof(VpaScope), 0);

  for (int32_t i = 0; i < (int32_t)darray_size(old); i++) {
    if (old[i].is_macro) {
      continue;
    }
    VpaScope s = old[i];
    old[i].name = NULL;
    old[i].children = NULL;
    old[i].leader = (VpaUnit){0};
    darray_push(fresh, s);
  }

  for (int32_t i = 0; i < (int32_t)darray_size(old); i++) {
    XFREE(old[i].name);
    _free_vpa_unit(&old[i].leader);
    for (int32_t j = 0; j < (int32_t)darray_size(old[i].children); j++) {
      _free_vpa_unit(&old[i].children[j]);
    }
    darray_del(old[i].children);
  }
  darray_del(old);

  ps->vpa_scopes = fresh;
  return true;
}

// --- Token set validation ---

static VpaScope* _find_scope_by_id(ParseState* ps, int32_t scope_id) {
  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_scopes); i++) {
    if (ps->vpa_scopes[i].scope_id == scope_id) {
      return &ps->vpa_scopes[i];
    }
  }
  return NULL;
}

static void _collect_post_end_tokens(VpaUnits units, int32_t** set) {
  for (int32_t i = 0; i < (int32_t)darray_size(units); i++) {
    VpaUnit* u = &units[i];
    if (u->kind != VPA_RE) {
      continue;
    }
    bool after_end = false;
    for (int32_t j = 0; j < (int32_t)darray_size(u->action_units); j++) {
      if (u->action_units[j] == -HOOK_ID_END) {
        after_end = true;
      } else if (after_end && u->action_units[j] > 0) {
        _id_set_add(set, u->action_units[j]);
      }
    }
  }
}

static void _collect_emit_set(VpaUnits units, int32_t** set, ParseState* ps, bool stop_at_end) {
  for (int32_t i = 0; i < (int32_t)darray_size(units); i++) {
    VpaUnit* u = &units[i];

    if (u->kind == VPA_RE) {
      for (int32_t j = 0; j < (int32_t)darray_size(u->action_units); j++) {
        if (stop_at_end && u->action_units[j] == -HOOK_ID_END) {
          break;
        }
        if (u->action_units[j] > 0) {
          _id_set_add(set, u->action_units[j]);
        }
      }
    } else if (u->kind == VPA_CALL) {
      VpaScope* callee = _find_scope_by_id(ps, u->call_scope_id);
      if (callee && callee->has_parser) {
        _id_set_add(set, u->call_scope_id);
      } else if (callee) {
        _collect_emit_set(callee->children, set, ps, false);
      }
      if (callee) {
        _collect_post_end_tokens(callee->children, set);
      }
    }
  }
}

static void _collect_peg_used_set(PegUnit* unit, int32_t** set, ParseState* ps, int32_t** visited_rules) {
  if (unit->kind == PEG_TERM) {
    _id_set_add(set, unit->id);
  }
  if (unit->kind == PEG_CALL) {
    if (!_id_set_has(*visited_rules, unit->id)) {
      _id_set_add(visited_rules, unit->id);
      const char* rn = symtab_get(&ps->rule_names, unit->id);
      VpaScope* vr = rn ? _find_scope(ps, rn) : NULL;
      if (vr) {
        _id_set_add(set, vr->scope_id);
      } else {
        PegRule* ref = rn ? _find_peg_rule(ps, rn) : NULL;
        if (ref) {
          _collect_peg_used_set(&ref->body, set, ps, visited_rules);
        }
      }
    }
  }
  if (unit->interlace_rhs_kind == PEG_TERM) {
    _id_set_add(set, unit->interlace_rhs_id);
  } else if (unit->interlace_rhs_kind == PEG_CALL) {
    if (!_id_set_has(*visited_rules, unit->interlace_rhs_id)) {
      _id_set_add(visited_rules, unit->interlace_rhs_id);
      const char* rn = symtab_get(&ps->rule_names, unit->interlace_rhs_id);
      VpaScope* vr = rn ? _find_scope(ps, rn) : NULL;
      if (vr) {
        _id_set_add(set, vr->scope_id);
      } else {
        PegRule* ref = rn ? _find_peg_rule(ps, rn) : NULL;
        if (ref) {
          _collect_peg_used_set(&ref->body, set, ps, visited_rules);
        }
      }
    }
  }
  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    _collect_peg_used_set(&unit->children[i], set, ps, visited_rules);
  }
}

static const char* _term_name(ParseState* ps, int32_t id) {
  if (id < symtab_count(&ps->scope_names)) {
    return symtab_get(&ps->scope_names, id);
  }
  return symtab_get(&ps->tokens, id);
}

static bool _is_ignored(ParseState* ps, const char* name) { return symtab_find(&ps->ignores.names, name) >= 0; }

static bool _validate_token_sets(ParseState* ps) {
  for (int32_t v = 0; v < (int32_t)darray_size(ps->vpa_scopes); v++) {
    VpaScope* scope = &ps->vpa_scopes[v];

    PegRule* entry = _find_peg_rule(ps, scope->name);
    if (!entry) {
      continue;
    }

    int32_t* emit_set = darray_new(sizeof(int32_t), 0);
    _collect_emit_set(scope->children, &emit_set, ps, true);

    // Leader tokens after .begin are emitted into the child scope
    VpaActionUnits lau = scope->leader.action_units;
    bool after_begin = false;
    for (int32_t i = 0; i < (int32_t)darray_size(lau); i++) {
      if (lau[i] == -HOOK_ID_BEGIN) {
        after_begin = true;
      } else if (after_begin && lau[i] > 0) {
        _id_set_add(&emit_set, lau[i]);
      }
    }

    int32_t* filtered_emit = darray_new(sizeof(int32_t), 0);
    for (int32_t i = 0; i < (int32_t)darray_size(emit_set); i++) {
      const char* name = _term_name(ps, emit_set[i]);
      if (!name || !_is_ignored(ps, name)) {
        darray_push(filtered_emit, emit_set[i]);
      }
    }
    darray_del(emit_set);

    int32_t* used_set = darray_new(sizeof(int32_t), 0);
    int32_t* visited_rules = darray_new(sizeof(int32_t), 0);
    _collect_peg_used_set(&entry->body, &used_set, ps, &visited_rules);
    darray_del(visited_rules);

    for (int32_t i = 0; i < (int32_t)darray_size(used_set); i++) {
      if (!_id_set_has(filtered_emit, used_set[i])) {
        parse_error(ps,
                    "scope '%s': peg uses token %s not emitted by vpa, did you forgot to add `@` for literal scopes?",
                    scope->name, _term_name(ps, used_set[i]));
        darray_del(filtered_emit);
        darray_del(used_set);
        return false;
      }
    }

    for (int32_t i = 0; i < (int32_t)darray_size(filtered_emit); i++) {
      if (!_id_set_has(used_set, filtered_emit[i])) {
        parse_error(ps, "scope '%s': vpa emits token %s not used by peg", scope->name,
                    _term_name(ps, filtered_emit[i]));
        darray_del(filtered_emit);
        darray_del(used_set);
        return false;
      }
    }

    darray_del(filtered_emit);
    darray_del(used_set);
  }
  return true;
}

// --- Left recursion detection ---

static bool _can_be_empty(PegUnit* unit) {
  if (unit->multiplier == '?' || unit->multiplier == '*') {
    return true;
  }
  if (unit->kind == PEG_BRANCHES) {
    for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
      if (_can_be_empty(&unit->children[i])) {
        return true;
      }
    }
  }
  if (unit->kind == PEG_SEQ) {
    for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
      if (!_can_be_empty(&unit->children[i])) {
        return false;
      }
    }
    return true;
  }
  return false;
}

static bool _is_nullable(ParseState* ps, PegUnit* unit, int32_t** visited) {
  if (unit->multiplier == '?' || unit->multiplier == '*') {
    return true;
  }
  switch (unit->kind) {
  case PEG_TERM:
    return false;
  case PEG_CALL: {
    if (_id_set_has(*visited, unit->id)) {
      return false;
    }
    _id_set_add(visited, unit->id);
    const char* rn = _rule_name(ps, unit->id);
    PegRule* ref = rn ? _find_peg_rule(ps, rn) : NULL;
    return ref && _is_nullable(ps, &ref->body, visited);
  }
  case PEG_BRANCHES:
    for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
      if (_is_nullable(ps, &unit->children[i], visited)) {
        return true;
      }
    }
    return false;
  case PEG_SEQ:
    for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
      if (!_is_nullable(ps, &unit->children[i], visited)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

static bool _check_interlace_loops(ParseState* ps, PegUnit* unit, const char* rule_name) {
  if (unit->interlace_rhs_kind != 0) {
    PegUnit lhs = {.kind = unit->kind, .id = unit->id};
    PegUnit rhs = {.kind = unit->interlace_rhs_kind, .id = unit->interlace_rhs_id};
    int32_t* v1 = darray_new(sizeof(int32_t), 0);
    int32_t* v2 = darray_new(sizeof(int32_t), 0);
    bool lhs_null = _is_nullable(ps, &lhs, &v1);
    bool rhs_null = _is_nullable(ps, &rhs, &v2);
    darray_del(v1);
    darray_del(v2);
    if (lhs_null && rhs_null) {
      parse_error(ps, "interlace in rule '%s': both lhs and rhs are nullable (infinite loop)", rule_name);
      return false;
    }
  }
  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    if (!_check_interlace_loops(ps, &unit->children[i], rule_name)) {
      return false;
    }
  }
  return true;
}

static bool _check_left_rec(ParseState* ps, PegUnit* unit, int32_t target_id, int32_t** visiting) {
  switch (unit->kind) {
  case PEG_CALL: {
    if (unit->id == target_id) {
      return true;
    }
    if (_id_set_has(*visiting, unit->id)) {
      break;
    }
    const char* rn = _rule_name(ps, unit->id);
    PegRule* ref = rn ? _find_peg_rule(ps, rn) : NULL;
    if (ref) {
      _id_set_add(visiting, unit->id);
      if (_check_left_rec(ps, &ref->body, target_id, visiting)) {
        return true;
      }
    }
    break;
  }
  case PEG_TERM:
    break;
  case PEG_BRANCHES:
    for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
      if (_check_left_rec(ps, &unit->children[i], target_id, visiting)) {
        return true;
      }
    }
    break;
  case PEG_SEQ:
    for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
      if (_check_left_rec(ps, &unit->children[i], target_id, visiting)) {
        return true;
      }
      if (!_can_be_empty(&unit->children[i])) {
        break;
      }
    }
    break;
  }
  return false;
}

static bool _check_undefined_calls(ParseState* ps, PegUnit* unit, const char* rule_name) {
  if (unit->kind == PEG_CALL) {
    const char* rn = _rule_name(ps, unit->id);
    if (!rn) {
      parse_error(ps, "rule '%s': call to unknown id %d", rule_name, unit->id);
      return false;
    }
    if (!_find_peg_rule(ps, rn) && !_find_scope(ps, rn)) {
      parse_error(ps, "rule '%s': called rule '%s' is not defined", rule_name, rn);
      return false;
    }
  }
  if (unit->interlace_rhs_kind == PEG_CALL) {
    const char* rn = _rule_name(ps, unit->interlace_rhs_id);
    if (!rn) {
      parse_error(ps, "rule '%s': interlace call to unknown id %d", rule_name, unit->interlace_rhs_id);
      return false;
    }
    if (!_find_peg_rule(ps, rn) && !_find_scope(ps, rn)) {
      parse_error(ps, "rule '%s': interlace called rule '%s' is not defined", rule_name, rn);
      return false;
    }
  }
  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    if (!_check_undefined_calls(ps, &unit->children[i], rule_name)) {
      return false;
    }
  }
  return true;
}

bool pp_detect_left_recursions(ParseState* ps) {
  for (int32_t r = 0; r < (int32_t)darray_size(ps->peg_rules); r++) {
    PegRule* rule = &ps->peg_rules[r];
    const char* rn = _rule_name(ps, rule->global_id);
    if (!rn) {
      continue;
    }
    if (!_check_undefined_calls(ps, &rule->body, rn)) {
      return false;
    }
  }

  for (int32_t r = 0; r < (int32_t)darray_size(ps->peg_rules); r++) {
    PegRule* rule = &ps->peg_rules[r];
    const char* rn = _rule_name(ps, rule->global_id);
    if (!rn) {
      continue;
    }
    int32_t* visiting = darray_new(sizeof(int32_t), 0);
    _id_set_add(&visiting, rule->global_id);
    bool found = _check_left_rec(ps, &rule->body, rule->global_id, &visiting);
    darray_del(visiting);
    if (found) {
      parse_error(ps, "left recursion detected in rule '%s'", rn);
      return false;
    }
  }

  for (int32_t r = 0; r < (int32_t)darray_size(ps->peg_rules); r++) {
    PegRule* rule = &ps->peg_rules[r];
    const char* rn = _rule_name(ps, rule->global_id);
    if (!rn) {
      continue;
    }
    if (!_check_interlace_loops(ps, &rule->body, rn)) {
      return false;
    }
  }

  return true;
}

// --- Auto-tagging ---

static const char* _unit_display_name(ParseState* ps, PegUnit* unit) {
  if (unit->kind == PEG_CALL) {
    return _rule_name(ps, unit->id);
  }
  if (unit->kind == PEG_TERM) {
    if (unit->id >= symtab_count(&ps->scope_names)) {
      return symtab_get(&ps->tokens, unit->id);
    } else {
      return symtab_get(&ps->scope_names, unit->id);
    }
  }
  return NULL;
}

static bool _auto_tag_unit(ParseState* ps, PegRule* rule, PegUnit* unit) {
  if (unit->kind == PEG_BRANCHES) {
    for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
      PegUnit* branch = &unit->children[i];
      if (!branch->tag || branch->tag[0] == '\0') {
        const char* dn = NULL;
        if (branch->kind == PEG_SEQ && (int32_t)darray_size(branch->children) > 0) {
          dn = _unit_display_name(ps, &branch->children[0]);
        } else {
          dn = _unit_display_name(ps, branch);
        }
        if (dn && dn[0]) {
          XFREE(branch->tag);
          branch->tag = strdup(dn);
        }
        if (!branch->tag || branch->tag[0] == '\0') {
          parse_error(ps, "branch in rule '%s' must have an explicit tag", _rule_name(ps, rule->global_id));
          return false;
        }
      }
    }
  }

  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    if (!_auto_tag_unit(ps, rule, &unit->children[i])) {
      return false;
    }
  }
  return true;
}

bool pp_auto_tag_branches(ParseState* ps) {
  for (int32_t r = 0; r < (int32_t)darray_size(ps->peg_rules); r++) {
    const char* rn = _rule_name(ps, ps->peg_rules[r].global_id);
    if (rn && strlen(rn) > 128) {
      parse_error(ps, "rule name '%s' exceeds 128 bytes", rn);
      return false;
    }
    if (!_auto_tag_unit(ps, &ps->peg_rules[r], &ps->peg_rules[r].body)) {
      return false;
    }
  }
  return true;
}

// --- C keyword check for tags ---

static const char* _c_keywords[] = {
    "alignas",
    "alignof",
    "auto",
    "bool",
    "break",
    "case",
    "char",
    "const",
    "constexpr",
    "continue",
    "default",
    "do",
    "double",
    "else",
    "enum",
    "extern",
    "false",
    "float",
    "for",
    "goto",
    "if",
    "inline",
    "int",
    "long",
    "nullptr",
    "register",
    "restrict",
    "return",
    "short",
    "signed",
    "sizeof",
    "static",
    "static_assert",
    "struct",
    "switch",
    "thread_local",
    "true",
    "typedef",
    "typeof",
    "typeof_unqual",
    "union",
    "unsigned",
    "void",
    "volatile",
    "while",
    "_Alignas",
    "_Alignof",
    "_Atomic",
    "_BitInt",
    "_Bool",
    "_Complex",
    "_Decimal128",
    "_Decimal32",
    "_Decimal64",
    "_Generic",
    "_Imaginary",
    "_Noreturn",
    "_Static_assert",
    "_Thread_local",
    NULL,
};

static bool _is_c_keyword(const char* name) {
  for (int32_t i = 0; _c_keywords[i]; i++) {
    if (strcmp(_c_keywords[i], name) == 0) {
      return true;
    }
  }
  return false;
}

static bool _check_keyword_tags(ParseState* ps, PegUnit* unit, const char* rn) {
  if (unit->tag && _is_c_keyword(unit->tag)) {
    parse_error(ps, "tag '%s' in rule '%s' is a C keyword", unit->tag, rn);
    return false;
  }
  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    if (!_check_keyword_tags(ps, &unit->children[i], rn)) {
      return false;
    }
  }
  return true;
}

static bool _check_dup_tags_in_branches(ParseState* ps, PegUnit* br, char*** tags, const char* rn) {
  for (int32_t j = 0; j < (int32_t)darray_size(br->children); j++) {
    char* tag = br->children[j].tag;
    if (!tag || !tag[0]) {
      continue;
    }
    for (int32_t k = 0; k < (int32_t)darray_size(*tags); k++) {
      if (strcmp((*tags)[k], tag) == 0) {
        parse_error(ps, "duplicate tag '%s' across bracket groups in rule '%s'", tag, rn);
        return false;
      }
    }
    darray_push(*tags, tag);
  }
  return true;
}

bool pp_check_duplicate_tags(ParseState* ps) {
  for (int32_t r = 0; r < (int32_t)darray_size(ps->peg_rules); r++) {
    PegRule* rule = &ps->peg_rules[r];
    const char* rn = _rule_name(ps, rule->global_id);

    // check C keyword tags
    if (!_check_keyword_tags(ps, &rule->body, rn)) {
      return false;
    }

    char** tags = darray_new(sizeof(char*), 0);
    bool ok = true;

    if (rule->body.kind == PEG_BRANCHES) {
      ok = _check_dup_tags_in_branches(ps, &rule->body, &tags, rn);
    } else {
      for (int32_t i = 0; i < (int32_t)darray_size(rule->body.children); i++) {
        PegUnit* child = &rule->body.children[i];
        if (child->kind != PEG_BRANCHES) {
          continue;
        }
        if (!_check_dup_tags_in_branches(ps, child, &tags, rn)) {
          ok = false;
          break;
        }
      }
    }
    darray_del(tags);
    if (!ok) {
      return false;
    }
  }
  return true;
}

// --- Helpers for hook resolution ---

static bool _au_has_hook(VpaActionUnits au, int32_t hook_id) {
  for (int32_t i = 0; i < (int32_t)darray_size(au); i++) {
    if (au[i] <= 0 && (-au[i]) == hook_id) {
      return true;
    }
  }
  return false;
}

static bool _unit_has_hook(VpaUnit* u, int32_t hook_id, ParseState* ps) {
  if (_au_has_hook(u->action_units, hook_id)) {
    return true;
  }
  for (int32_t i = 0; i < (int32_t)darray_size(u->action_units); i++) {
    if (u->action_units[i] <= 0) {
      int32_t hid = -u->action_units[i];
      if (hid >= HOOK_ID_BUILTIN_COUNT) {
        for (int32_t e = 0; e < (int32_t)darray_size(ps->effect_decls); e++) {
          if (ps->effect_decls[e].hook_id == hid) {
            for (int32_t ef = 0; ef < (int32_t)darray_size(ps->effect_decls[e].effects); ef++) {
              if (ps->effect_decls[e].effects[ef] <= 0 && (-ps->effect_decls[e].effects[ef]) == hook_id) {
                return true;
              }
            }
          }
        }
      }
    }
  }
  return false;
}

// --- vpa scope validity ---

bool pp_validate_vpa_scopes(ParseState* ps) {
  bool has_main = false;
  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_scopes); i++) {
    VpaScope* scope = &ps->vpa_scopes[i];
    if (strcmp(scope->name, "main") == 0) {
      has_main = true;
    }
    for (int32_t j = 0; j < i; j++) {
      if (strcmp(ps->vpa_scopes[j].name, scope->name) == 0) {
        parse_error(ps, "duplicate vpa scope name '%s'", scope->name);
        return false;
      }
    }
  }
  if (!has_main) {
    parse_error(ps, "'main' rule must exist in [[vpa]]");
    return false;
  }

  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_scopes); i++) {
    VpaScope* scope = &ps->vpa_scopes[i];
    bool is_main = strcmp(scope->name, "main") == 0;

    if (!is_main && _has_leader(scope)) {
      if ((int32_t)darray_size(scope->leader.re) == 0) {
        parse_error(ps, "scope '%s': leading pattern must match at least 1 character", scope->name);
        return false;
      }
    }

    if (!is_main) {
      bool has_begin = false, has_end = false;
      if (_unit_has_hook(&scope->leader, HOOK_ID_BEGIN, ps)) {
        has_begin = true;
      }
      if (_unit_has_hook(&scope->leader, HOOK_ID_END, ps)) {
        has_end = true;
      }
      for (int32_t j = 0; j < (int32_t)darray_size(scope->children); j++) {
        if (_unit_has_hook(&scope->children[j], HOOK_ID_BEGIN, ps)) {
          has_begin = true;
        }
        if (_unit_has_hook(&scope->children[j], HOOK_ID_END, ps)) {
          has_end = true;
        }
      }
      if (has_begin && !has_end) {
        parse_error(ps, "scope '%s' missing .end", scope->name);
        return false;
      }
      if (!has_begin && has_end) {
        parse_error(ps, "scope '%s' missing .begin", scope->name);
        return false;
      }
      if (!has_begin && !has_end) {
        parse_error(ps, "scope '%s' missing .begin and .end", scope->name);
        return false;
      }
    }

    int32_t empty_count = 0;
    for (int32_t j = 0; j < (int32_t)darray_size(scope->children); j++) {
      VpaUnit* u = &scope->children[j];
      if (u->kind != VPA_RE) {
        continue;
      }
      if ((int32_t)darray_size(u->re) == 0) {
        empty_count++;
        if (empty_count > 1) {
          parse_error(ps, "scope '%s': at most 1 empty pattern allowed inside scope", scope->name);
          return false;
        }
        if (!_unit_has_hook(u, HOOK_ID_END, ps) && !_unit_has_hook(u, HOOK_ID_FAIL, ps)) {
          parse_error(ps, "scope '%s': empty pattern must have .end or .fail", scope->name);
          return false;
        }
      }
    }
  }

  return true;
}

// --- peg rule validity ---

bool pp_validate_peg_rules(ParseState* ps) {
  bool has_main = false;
  for (int32_t i = 0; i < (int32_t)darray_size(ps->peg_rules); i++) {
    const char* rn = _rule_name(ps, ps->peg_rules[i].global_id);
    if (rn && strcmp(rn, "main") == 0) {
      has_main = true;
    }
    for (int32_t j = 0; j < i; j++) {
      const char* rn2 = _rule_name(ps, ps->peg_rules[j].global_id);
      if (rn && rn2 && strcmp(rn, rn2) == 0) {
        parse_error(ps, "duplicate peg rule name '%s'", rn);
        return false;
      }
    }
  }
  if (!has_main) {
    parse_error(ps, "'main' rule must exist in [[peg]]");
    return false;
  }
  return true;
}

// --- vpa & peg scope matching ---

bool pp_match_scopes(ParseState* ps) {
  for (int32_t p = 0; p < (int32_t)darray_size(ps->peg_rules); p++) {
    const char* rn = _rule_name(ps, ps->peg_rules[p].global_id);
    if (!rn) {
      continue;
    }
    VpaScope* vr = _find_scope(ps, rn);
    if (vr) {
      vr->has_parser = true;
      ps->peg_rules[p].scope_id = vr->scope_id;
    }
  }
  VpaScope* main_scope = _find_scope(ps, "main");
  if (main_scope && !main_scope->has_parser) {
    parse_error(ps, "'main' scope must have a parser (missing [[peg]] main rule)");
    return false;
  }
  if (!_validate_token_sets(ps)) {
    return false;
  }

  return true;
}
