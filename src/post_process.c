// specs/post_process.md
#include "post_process.h"
#include "darray.h"
#include "peg.h"
#include "symtab.h"
#include "ustr.h"
#include "vpa.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- VpaUnit helpers ---

static void _free_vpa_unit(VpaUnit* u) {
  re_ir_free(u->re);
  darray_del(u->action_units);
  free(u->macro_name);
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

// --- String set utilities ---

static bool _str_set_has(char** set, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(set); i++) {
    if (strcmp(set[i], name) == 0) {
      return true;
    }
  }
  return false;
}

static void _str_set_add(char*** set, const char* name) {
  if (!_str_set_has(*set, name)) {
    char* dup = strdup(name);
    darray_push(*set, dup);
  }
}

static void _str_set_free(char** set) {
  for (int32_t i = 0; i < (int32_t)darray_size(set); i++) {
    free(set[i]);
  }
  darray_del(set);
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


static const char* _rule_name(ParseState* ps, int32_t global_id) {
  return symtab_get(&ps->rule_names, global_id);
}

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
    free(old[i].name);
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

static void _collect_emit_set(ParseState* ps, VpaUnits units, char*** set) {
  for (int32_t i = 0; i < (int32_t)darray_size(units); i++) {
    VpaUnit* u = &units[i];

    if (u->kind == VPA_RE) {
      for (int32_t j = 0; j < (int32_t)darray_size(u->action_units); j++) {
        if (u->action_units[j] > 0) {
          const char* tok_name = symtab_get(&ps->tokens, u->action_units[j]);
          if (tok_name) {
            _str_set_add(set, tok_name);
          }
        }
      }
    } else if (u->kind == VPA_CALL) {
      const char* ref_name = symtab_get(&ps->scope_names, u->call_scope_id);
      VpaScope* ref = ref_name ? _find_scope(ps, ref_name) : NULL;
      if (ref) {
        _str_set_add(set, ref_name);
      } else if (ref_name) {
        _str_set_add(set, ref_name);
      }
    }
  }
}

static void _collect_peg_used_set(PegUnit* unit, char*** set, ParseState* ps, char*** visited_rules) {
  if (unit->kind == PEG_TOK) {
    const char* tok_name = symtab_get(&ps->tokens, unit->id);
    if (tok_name) {
      _str_set_add(set, tok_name);
    }
  }
  if (unit->kind == PEG_CALL) {
    const char* rn = symtab_get(&ps->rule_names, unit->id);
    if (rn && !_str_set_has(*visited_rules, rn)) {
      VpaScope* vr = _find_scope(ps, rn);
      if (vr) {
        _str_set_add(set, rn);
      }
    }
  }
  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    _collect_peg_used_set(&unit->children[i], set, ps, visited_rules);
  }
}

static bool _is_ignored(ParseState* ps, const char* name) { return symtab_find(&ps->ignores.names, name) >= 0; }

static bool _validate_token_sets(ParseState* ps) {
  for (int32_t v = 0; v < (int32_t)darray_size(ps->vpa_scopes); v++) {
    VpaScope* scope = &ps->vpa_scopes[v];

    PegRule* entry = _find_peg_rule(ps, scope->name);
    if (!entry) {
      continue;
    }

    char** emit_set = darray_new(sizeof(char*), 0);
    _collect_emit_set(ps, scope->children, &emit_set);

    char** filtered_emit = darray_new(sizeof(char*), 0);
    for (int32_t i = 0; i < (int32_t)darray_size(emit_set); i++) {
      if (!_is_ignored(ps, emit_set[i])) {
        char* dup = strdup(emit_set[i]);
        darray_push(filtered_emit, dup);
      }
    }
    _str_set_free(emit_set);

    char** used_set = darray_new(sizeof(char*), 0);
    char** visited_rules = darray_new(sizeof(char*), 0);
    _collect_peg_used_set(&entry->seq, &used_set, ps, &visited_rules);
    _str_set_free(visited_rules);

    for (int32_t i = 0; i < (int32_t)darray_size(used_set); i++) {
      if (!_str_set_has(filtered_emit, used_set[i])) {
        parse_error(ps, "scope '%s': peg uses token @%s not emitted by vpa", scope->name, used_set[i]);
        _str_set_free(filtered_emit);
        _str_set_free(used_set);
        return false;
      }
    }

    for (int32_t i = 0; i < (int32_t)darray_size(filtered_emit); i++) {
      if (!_str_set_has(used_set, filtered_emit[i])) {
        parse_error(ps, "scope '%s': vpa emits token @%s not used by peg", scope->name, filtered_emit[i]);
        _str_set_free(filtered_emit);
        _str_set_free(used_set);
        return false;
      }
    }

    _str_set_free(filtered_emit);
    _str_set_free(used_set);
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
      if ((int32_t)darray_size(unit->children[i].children) == 0) {
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

static bool _check_left_rec(ParseState* ps, PegUnit* unit, const char* target, char*** visiting) {
  switch (unit->kind) {
  case PEG_CALL: {
    const char* rn = _rule_name(ps, unit->id);
    if (!rn) {
      break;
    }
    if (strcmp(rn, target) == 0) {
      return true;
    }
    if (_str_set_has(*visiting, rn)) {
      break;
    }
    PegRule* ref = _find_peg_rule(ps, rn);
    if (ref) {
      _str_set_add(visiting, rn);
      if (_check_left_rec(ps, &ref->seq, target, visiting)) {
        return true;
      }
    }
    break;
  }
  case PEG_TOK:
    break;
  case PEG_BRANCHES:
    for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
      if (_check_left_rec(ps, &unit->children[i], target, visiting)) {
        return true;
      }
    }
    break;
  case PEG_SEQ:
    for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
      if (_check_left_rec(ps, &unit->children[i], target, visiting)) {
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

bool pp_detect_left_recursions(ParseState* ps) {
  for (int32_t r = 0; r < (int32_t)darray_size(ps->peg_rules); r++) {
    PegRule* rule = &ps->peg_rules[r];
    const char* rn = _rule_name(ps, rule->global_id);
    if (!rn) {
      continue;
    }
    char** visiting = darray_new(sizeof(char*), 0);
    _str_set_add(&visiting, rn);
    bool found = _check_left_rec(ps, &rule->seq, rn, &visiting);
    _str_set_free(visiting);
    if (found) {
      parse_error(ps, "left recursion detected in rule '%s'", rn);
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
  if (unit->kind == PEG_TOK) {
    return symtab_get(&ps->tokens, unit->id);
  }
  return NULL;
}

static bool _auto_tag_unit(ParseState* ps, PegRule* rule, PegUnit* unit) {
  if (unit->kind == PEG_BRANCHES) {
    for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
      PegUnit* branch = &unit->children[i];
      if (!branch->tag || branch->tag[0] == '\0') {
        if ((int32_t)darray_size(branch->children) > 0) {
          PegUnit* first = &branch->children[0];
          const char* dn = _unit_display_name(ps, first);
          if (dn && dn[0]) {
            free(branch->tag);
            branch->tag = strdup(dn);
          }
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
    if (!_auto_tag_unit(ps, &ps->peg_rules[r], &ps->peg_rules[r].seq)) {
      return false;
    }
  }
  return true;
}

bool pp_check_duplicate_tags(ParseState* ps) {
  for (int32_t r = 0; r < (int32_t)darray_size(ps->peg_rules); r++) {
    PegRule* rule = &ps->peg_rules[r];
    char** tags = darray_new(sizeof(char*), 0);

    for (int32_t i = 0; i < (int32_t)darray_size(rule->seq.children); i++) {
      PegUnit* child = &rule->seq.children[i];
      if (child->kind != PEG_BRANCHES) {
        continue;
      }
      for (int32_t j = 0; j < (int32_t)darray_size(child->children); j++) {
        char* tag = child->children[j].tag;
        if (!tag || !tag[0]) {
          continue;
        }
        for (int32_t k = 0; k < (int32_t)darray_size(tags); k++) {
          if (strcmp(tags[k], tag) == 0) {
            parse_error(ps, "duplicate tag '%s' across bracket groups in rule '%s'", tag,
                        _rule_name(ps, rule->global_id));
            darray_del(tags);
            return false;
          }
        }
        darray_push(tags, tag);
      }
    }
    darray_del(tags);
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
