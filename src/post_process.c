// specs/post_process.md
#include "post_process.h"
#include "darray.h"
#include "peg.h"
#include "ustr.h"
#include "vpa.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Helper functions ---

static bool _is_vpa_scope_rule(VpaRule* rule) {
  if (rule->is_macro) {
    return false;
  }
  if (rule->is_scope) {
    return true;
  }
  for (int32_t i = 0; i < (int32_t)darray_size(rule->units); i++) {
    if (rule->units[i].kind == VPA_SCOPE) {
      return true;
    }
  }
  return false;
}

static VpaUnit* _get_scope_body(VpaRule* rule) {
  if (rule->is_scope) {
    return rule->units;
  }
  for (int32_t i = 0; i < (int32_t)darray_size(rule->units); i++) {
    if (rule->units[i].kind == VPA_SCOPE) {
      return rule->units[i].children;
    }
  }
  return NULL;
}

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

static VpaRule* _find_vpa_rule(ParseState* ps, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules); i++) {
    if (!ps->vpa_rules[i].is_macro && strcmp(ps->vpa_rules[i].name, name) == 0) {
      return &ps->vpa_rules[i];
    }
  }
  return NULL;
}

static PegRule* _find_peg_rule(ParseState* ps, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(ps->peg_rules); i++) {
    if (strcmp(ps->peg_rules[i].name, name) == 0) {
      return &ps->peg_rules[i];
    }
  }
  return NULL;
}

static void _expand_kw_in_peg_unit(PegUnit* unit, ParseState* ps) {
  if (unit->kind == PEG_TOK && unit->name) {
    for (int32_t k = 0; k < (int32_t)darray_size(ps->keywords); k++) {
      KeywordEntry* kw = &ps->keywords[k];
      char* lit = ustr_slice(ps->src, kw->lit_off, kw->lit_off + kw->lit_len);
      if (strcmp(unit->name, lit) == 0) {
        parse_set_str(&unit->name, parse_sfmt("%s.%s", kw->group, lit));
        ustr_del(lit);
        break;
      }
      ustr_del(lit);
    }
  }
  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    _expand_kw_in_peg_unit(&unit->children[i], ps);
  }
  if (unit->interlace) {
    _expand_kw_in_peg_unit(unit->interlace, ps);
  }
}

// --- Post-processing: inline macros ---

static VpaRule* _find_macro(ParseState* ps, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules); i++) {
    if (ps->vpa_rules[i].is_macro && strcmp(ps->vpa_rules[i].name, name) == 0) {
      return &ps->vpa_rules[i];
    }
  }
  return NULL;
}

static VpaUnit _clone_vpa_unit(VpaUnit* src) {
  VpaUnit dst = *src;
  dst.name = src->name ? strdup(src->name) : NULL;
  dst.user_hook = src->user_hook ? strdup(src->user_hook) : NULL;
  dst.re = re_ir_clone(src->re);
  if ((int32_t)darray_size(src->children) > 0) {
    dst.children = darray_new(sizeof(VpaUnit), darray_size(src->children));
    for (int32_t i = 0; i < (int32_t)darray_size(src->children); i++) {
      dst.children[i] = _clone_vpa_unit(&src->children[i]);
    }
  }
  return dst;
}

static void _add_vpa_unit(VpaRule* rule, VpaUnit unit) {
  if (!rule->units) {
    rule->units = darray_new(sizeof(VpaUnit), 0);
  }
  darray_push(rule->units, unit);
}

static void _free_vpa_unit(VpaUnit* unit) {
  re_ir_free(unit->re);
  free(unit->name);
  free(unit->user_hook);
  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    _free_vpa_unit(&unit->children[i]);
  }
  darray_del(unit->children);
}

bool pp_inline_macros(ParseState* ps) {
  // TODO: return macro not found error
  for (int32_t r = 0; r < (int32_t)darray_size(ps->vpa_rules); r++) {
    VpaRule* rule = &ps->vpa_rules[r];
    if (rule->is_macro) {
      continue;
    }
    for (int32_t i = 0; i < (int32_t)darray_size(rule->units); i++) {
      VpaUnit* unit = &rule->units[i];
      if (unit->kind != VPA_REF || !unit->name || unit->name[0] != '*') {
        continue;
      }
      VpaRule* macro = _find_macro(ps, unit->name + 1);
      if (!macro) {
        continue;
      }

      int32_t new_count = (int32_t)darray_size(rule->units) - 1 + (int32_t)darray_size(macro->units);
      int32_t tail = (int32_t)darray_size(rule->units) - i - 1;

      _free_vpa_unit(unit);

      rule->units = darray_grow(rule->units, (size_t)new_count);

      if (tail > 0 && (int32_t)darray_size(macro->units) != 1) {
        memmove(&rule->units[i + (int32_t)darray_size(macro->units)], &rule->units[i + 1],
                (size_t)tail * sizeof(VpaUnit));
      }

      for (int32_t m = 0; m < (int32_t)darray_size(macro->units); m++) {
        rule->units[i + m] = _clone_vpa_unit(&macro->units[m]);
      }
      i += (int32_t)darray_size(macro->units) - 1;
    }
  }
  return true;
}

bool pp_expand_keywords(ParseState* ps) {
  // TODO: report keyword not found error

  for (int32_t k = 0; k < (int32_t)darray_size(ps->keywords); k++) {
    KeywordEntry* kw = &ps->keywords[k];
    char* lit = ustr_slice(ps->src, kw->lit_off, kw->lit_off + kw->lit_len);

    char* tok_name = parse_sfmt("%s.%s", kw->group, lit);
    ustr_del(lit);
    ReIr ir = re_ir_build_literal(ps->src, kw->lit_off, kw->lit_len);

    bool added = false;
    for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules) && !added; i++) {
      VpaRule* rule = &ps->vpa_rules[i];
      if (rule->is_macro || !rule->is_scope) {
        continue;
      }
      for (int32_t j = 0; j < (int32_t)darray_size(rule->units); j++) {
        if (rule->units[j].kind == VPA_REF && rule->units[j].name && strcmp(rule->units[j].name, kw->group) == 0) {
          VpaUnit unit = {.kind = VPA_REGEXP, .re = ir, .name = strdup(tok_name)};
          _add_vpa_unit(rule, unit);
          added = true;
          break;
        }
      }
    }

    if (!added) {
      for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules); i++) {
        if (strcmp(ps->vpa_rules[i].name, kw->group) == 0) {
          VpaUnit unit = {.kind = VPA_REGEXP, .re = ir, .name = strdup(tok_name)};
          _add_vpa_unit(&ps->vpa_rules[i], unit);
          added = true;
          break;
        }
      }
    }

    if (!added) {
      re_ir_free(ir);
    }
    free(tok_name);
  }

  for (int32_t p = 0; p < (int32_t)darray_size(ps->peg_rules); p++) {
    _expand_kw_in_peg_unit(&ps->peg_rules[p].seq, ps);
  }
  return true;
}

static void _collect_emit_set(ParseState* ps, VpaUnit* units, char*** set, char*** visited) {
  for (int32_t i = 0; i < (int32_t)darray_size(units); i++) {
    VpaUnit* u = &units[i];

    if (u->kind == VPA_REGEXP) {
      if (u->name && u->name[0]) {
        _str_set_add(set, u->name);
      }
    } else if (u->kind == VPA_REF) {
      VpaRule* ref = u->name ? _find_vpa_rule(ps, u->name) : NULL;
      if (ref) {
        if (!_is_vpa_scope_rule(ref)) {
          if (!_str_set_has(*visited, u->name)) {
            char* dup = strdup(u->name);
            darray_push(*visited, dup);
            _collect_emit_set(ps, ref->units, set, visited);
            for (int32_t j = 0; j < (int32_t)darray_size(ref->units); j++) {
              VpaUnit* ru = &ref->units[j];
              if ((ru->kind == VPA_REGEXP) && (!ru->name || !ru->name[0])) {
                _str_set_add(set, ref->name);
              }
            }
          }
        }
      } else if (u->name && u->name[0]) {
        _str_set_add(set, u->name);
      }
    }
  }
}

static void _collect_peg_used_set(PegUnit* unit, char*** set, ParseState* ps, char*** visited_rules) {
  if ((unit->kind == PEG_TOK || unit->kind == PEG_KEYWORD_TOK) && unit->name) {
    _str_set_add(set, unit->name);
  }
  if (unit->kind == PEG_ID && unit->name) {
    if (!_str_set_has(*visited_rules, unit->name)) {
      VpaRule* vr = _find_vpa_rule(ps, unit->name);
      if (!vr || !_is_vpa_scope_rule(vr)) {
        char* dup = strdup(unit->name);
        darray_push(*visited_rules, dup);
        PegRule* ref = _find_peg_rule(ps, unit->name);
        if (ref) {
          _collect_peg_used_set(&ref->seq, set, ps, visited_rules);
        }
      }
    }
  }
  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    _collect_peg_used_set(&unit->children[i], set, ps, visited_rules);
  }
  if (unit->interlace) {
    _collect_peg_used_set(unit->interlace, set, ps, visited_rules);
  }
}

static bool _is_ignored(ParseState* ps, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(ps->ignores.names); i++) {
    if (strcmp(ps->ignores.names[i], name) == 0) {
      return true;
    }
  }
  return false;
}

static bool _validate_token_sets(ParseState* ps) {
  for (int32_t v = 0; v < (int32_t)darray_size(ps->vpa_rules); v++) {
    VpaRule* vpa_rule = &ps->vpa_rules[v];
    if (!_is_vpa_scope_rule(vpa_rule)) {
      continue;
    }
    VpaUnit* body = _get_scope_body(vpa_rule);
    if (!body) {
      continue;
    }

    bool has_peg = false;
    for (int32_t p = 0; p < (int32_t)darray_size(ps->peg_rules); p++) {
      if (strcmp(ps->peg_rules[p].name, vpa_rule->name) == 0) {
        has_peg = true;
        break;
      }
    }
    if (!has_peg) {
      continue;
    }

    char** emit_set = darray_new(sizeof(char*), 0);
    char** visited = darray_new(sizeof(char*), 0);
    _collect_emit_set(ps, body, &emit_set, &visited);
    _str_set_free(visited);

    char** filtered_emit = darray_new(sizeof(char*), 0);
    for (int32_t i = 0; i < (int32_t)darray_size(emit_set); i++) {
      if (!_is_ignored(ps, emit_set[i])) {
        char* dup = strdup(emit_set[i]);
        darray_push(filtered_emit, dup);
      }
    }
    _str_set_free(emit_set);

    char** used_set = darray_new(sizeof(char*), 0);
    PegRule* entry = _find_peg_rule(ps, vpa_rule->name);
    if (entry) {
      char** visited_rules = darray_new(sizeof(char*), 0);
      _collect_peg_used_set(&entry->seq, &used_set, ps, &visited_rules);
      _str_set_free(visited_rules);
    }

    for (int32_t i = 0; i < (int32_t)darray_size(used_set); i++) {
      if (!_str_set_has(filtered_emit, used_set[i])) {
        parse_error(ps, "scope '%s': peg uses token @%s not emitted by vpa", vpa_rule->name, used_set[i]);
        _str_set_free(filtered_emit);
        _str_set_free(used_set);
        return false;
      }
    }

    for (int32_t i = 0; i < (int32_t)darray_size(filtered_emit); i++) {
      if (!_str_set_has(used_set, filtered_emit[i])) {
        parse_error(ps, "scope '%s': vpa emits token @%s not used by peg", vpa_rule->name, filtered_emit[i]);
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
  case PEG_ID:
    if (!unit->name) {
      break;
    }
    if (strcmp(unit->name, target) == 0) {
      return true;
    }
    if (_str_set_has(*visiting, unit->name)) {
      break;
    }
    {
      PegRule* ref = _find_peg_rule(ps, unit->name);
      if (ref) {
        _str_set_add(visiting, unit->name);
        if (_check_left_rec(ps, &ref->seq, target, visiting)) {
          return true;
        }
      }
    }
    break;
  case PEG_TOK:
  case PEG_KEYWORD_TOK:
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
    char** visiting = darray_new(sizeof(char*), 0);
    _str_set_add(&visiting, rule->name);
    bool found = _check_left_rec(ps, &rule->seq, rule->name, &visiting);
    _str_set_free(visiting);
    if (found) {
      parse_error(ps, "left recursion detected in rule '%s'", rule->name);
      return false;
    }
  }
  return true;
}

// --- Auto-tagging ---

static bool _auto_tag_unit(ParseState* ps, PegRule* rule, PegUnit* unit) {
  if (unit->kind == PEG_BRANCHES) {
    for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
      PegUnit* branch = &unit->children[i];
      if (!branch->tag || branch->tag[0] == '\0') {
        if ((int32_t)darray_size(branch->children) > 0) {
          PegUnit* first = &branch->children[0];
          if ((first->kind == PEG_ID || first->kind == PEG_TOK) && first->name && first->name[0]) {
            free(branch->tag);
            branch->tag = strdup(first->name);
          }
        }
        if (!branch->tag || branch->tag[0] == '\0') {
          parse_error(ps, "branch in rule '%s' must have an explicit tag", rule->name);
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
  if (unit->interlace) {
    if (!_auto_tag_unit(ps, rule, unit->interlace)) {
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
            parse_error(ps, "duplicate tag '%s' across bracket groups in rule '%s'", tag, rule->name);
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

bool pp_validate(ParseState* ps) {
  bool has_vpa_main = false;
  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules); i++) {
    if (!ps->vpa_rules[i].is_macro && strcmp(ps->vpa_rules[i].name, "main") == 0) {
      has_vpa_main = true;
      break;
    }
  }
  if (!has_vpa_main) {
    parse_error(ps, "'main' rule must exist in [[vpa]]");
    return false;
  }

  bool has_peg_main = false;
  for (int32_t i = 0; i < (int32_t)darray_size(ps->peg_rules); i++) {
    if (strcmp(ps->peg_rules[i].name, "main") == 0) {
      has_peg_main = true;
      break;
    }
  }
  if (!has_peg_main) {
    parse_error(ps, "'main' rule must exist in [[peg]]");
    return false;
  }

  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules); i++) {
    VpaRule* rule = &ps->vpa_rules[i];
    if (!rule->is_scope || rule->is_macro || strcmp(rule->name, "main") == 0) {
      continue;
    }
    bool has_begin = false;
    bool has_end = false;
    for (int32_t j = 0; j < (int32_t)darray_size(rule->units); j++) {
      VpaUnit* u = &rule->units[j];
      if (u->hook == TOK_HOOK_BEGIN) {
        has_begin = true;
      }
      if (u->hook == TOK_HOOK_END) {
        has_end = true;
      }
      if (u->user_hook && u->user_hook[0]) {
        for (int32_t e = 0; e < (int32_t)darray_size(ps->effects); e++) {
          if (strcmp(ps->effects[e].hook_name, u->user_hook) == 0) {
            for (int32_t ef = 0; ef < (int32_t)darray_size(ps->effects[e].effects); ef++) {
              if (ps->effects[e].effects[ef] == TOK_HOOK_BEGIN) {
                has_begin = true;
              }
              if (ps->effects[e].effects[ef] == TOK_HOOK_END) {
                has_end = true;
              }
            }
          }
        }
      }
    }
    if (has_begin && !has_end) {
      parse_error(ps, "scope '%s' missing .end", rule->name);
      return false;
    }
    if (!has_begin && has_end) {
      parse_error(ps, "scope '%s' missing .begin", rule->name);
      return false;
    }
    if (!has_begin && !has_end) {
      parse_error(ps, "scope '%s' missing .begin and .end", rule->name);
      return false;
    }
  }

  if (!_validate_token_sets(ps)) {
    return false;
  }

  return true;
}
