// VPA (Visibly Pushdown Automata) code generation.
//
// Generates per-scope DFA lexer functions in LLVM IR,
// an action dispatch function (switch on action_id -> micro-ops),
// an outer lexing loop with scope stack management,
// and emits runtime data structures and token ID definitions to the C header.

#include "vpa.h"
#include "aut.h"
#include "darray.h"
#include "parse.h"
#include "re.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Action registry ---

typedef struct {
  int32_t tok_id;       // 0 when no token should be emitted
  int32_t push_scope_id; // -1 when no scope should be pushed
  bool pop_scope;
  int32_t hook;
  char* user_hook;
  char* parse_scope_name;
} ActionEntry;

typedef struct {
  ActionEntry* entries; // darray (action_id = index + 1)
  char** tok_names;     // darray, parallel
} ActionRegistry;

static int32_t _register_action(ActionRegistry* reg, int32_t tok_id, int32_t push_scope_id, bool pop_scope, int32_t hook,
                                const char* user_hook, const char* parse_scope_name) {
  int32_t id = (int32_t)darray_size(reg->entries) + 1;
  ActionEntry e = {
      .tok_id = tok_id,
      .push_scope_id = push_scope_id,
      .pop_scope = pop_scope,
      .hook = hook,
      .user_hook = user_hook ? strdup(user_hook) : NULL,
      .parse_scope_name = parse_scope_name ? strdup(parse_scope_name) : NULL,
  };
  darray_push(reg->entries, e);
  darray_push(reg->tok_names, NULL);
  return id;
}

static int32_t _register_token(ActionRegistry* reg, const char* tok_name) {
  int32_t count = (int32_t)darray_size(reg->entries);
  for (int32_t i = 0; i < count; i++) {
    if (reg->tok_names[i] && strcmp(reg->tok_names[i], tok_name) == 0) {
      return i + 1;
    }
  }
  int32_t tok_id = count + 1;
  ActionEntry e = {
      .tok_id = tok_id,
      .push_scope_id = -1,
      .pop_scope = false,
      .hook = 0,
      .user_hook = NULL,
      .parse_scope_name = NULL,
  };
  darray_push(reg->entries, e);
  darray_push(reg->tok_names, strdup(tok_name));
  return tok_id;
}

static void _free_action_registry(ActionRegistry* reg) {
  int32_t count = (int32_t)darray_size(reg->entries);
  for (int32_t i = 0; i < count; i++) {
    free(reg->entries[i].user_hook);
    free(reg->entries[i].parse_scope_name);
    free(reg->tok_names[i]);
  }
  darray_del(reg->entries);
  darray_del(reg->tok_names);
}

// --- Scope info ---

typedef struct {
  char* name;
  int32_t scope_id;
  VpaUnit* body;         // NOT owned
  ReAstNode* leader_ast; // NOT owned
  int32_t leader_hook;
  char* leader_user_hook;
} ScopeInfo;

// --- DFA pattern ---

typedef struct {
  VpaUnitKind kind;
  ReAstNode* ast;
  const char* state_name;
  int32_t action_id;
} DfaPattern;

// --- Helpers ---

static VpaRule* _find_rule(VpaRule* rules, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(rules); i++) {
    if (strcmp(rules[i].name, name) == 0) {
      return &rules[i];
    }
  }
  return NULL;
}

static VpaUnit* _find_scope_unit(VpaRule* rule) {
  for (int32_t i = 0; i < (int32_t)darray_size(rule->units); i++) {
    if (rule->units[i].kind == VPA_SCOPE) {
      return &rule->units[i];
    }
  }
  return NULL;
}

static ScopeInfo* _find_scope(ScopeInfo* scopes, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    if (strcmp(scopes[i].name, name) == 0) {
      return &scopes[i];
    }
  }
  return NULL;
}

static bool _find_state(StateDecl* states, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(states); i++) {
    if (strcmp(states[i].name, name) == 0) {
      return true;
    }
  }
  return false;
}

static bool _scope_has_peg(PegRule* peg_rules, const char* scope_name) {
  for (int32_t i = 0; i < (int32_t)darray_size(peg_rules); i++) {
    if (strcmp(peg_rules[i].name, scope_name) == 0) {
      return true;
    }
  }
  return false;
}

static bool _hook_has_effect(EffectDecl* effects, const char* hook_name, int32_t effect) {
  if (!hook_name || !hook_name[0]) {
    return false;
  }
  for (int32_t i = 0; i < (int32_t)darray_size(effects); i++) {
    if (strcmp(effects[i].hook_name, hook_name) != 0) {
      continue;
    }
    for (int32_t j = 0; j < (int32_t)darray_size(effects[i].effects); j++) {
      if (effects[i].effects[j] == effect) {
        return true;
      }
    }
  }
  return false;
}

static bool _has_user_hook(ActionEntry* entry) { return entry->user_hook && entry->user_hook[0]; }

static bool _has_parse_scope(ActionEntry* entry) { return entry->parse_scope_name && entry->parse_scope_name[0]; }

static bool _is_first_user_hook(ActionRegistry* reg, int32_t idx) {
  if (!_has_user_hook(&reg->entries[idx])) {
    return false;
  }
  for (int32_t i = 0; i < idx; i++) {
    if (_has_user_hook(&reg->entries[i]) && strcmp(reg->entries[i].user_hook, reg->entries[idx].user_hook) == 0) {
      return false;
    }
  }
  return true;
}

static void _make_user_hook_symbol(const char* user_hook, char* out, size_t out_sz) {
  const char* hook_name = user_hook && user_hook[0] == '.' ? user_hook + 1 : user_hook;
  snprintf(out, out_sz, "vpa_hook_%s", hook_name ? hook_name : "");
}

static bool _is_first_parse_scope(ActionRegistry* reg, int32_t idx) {
  if (!_has_parse_scope(&reg->entries[idx])) {
    return false;
  }
  for (int32_t i = 0; i < idx; i++) {
    if (_has_parse_scope(&reg->entries[i]) &&
        strcmp(reg->entries[i].parse_scope_name, reg->entries[idx].parse_scope_name) == 0) {
      return false;
    }
  }
  return true;
}

static void _make_parse_scope_symbol(const char* scope_name, char* out, size_t out_sz) {
  snprintf(out, out_sz, "vpa_parse_%s", scope_name ? scope_name : "");
}

static void _make_state_match_symbol(const char* state_name, char* out, size_t out_sz) {
  snprintf(out, out_sz, "match_%s", state_name ? state_name : "");
}

// --- Walk ReAstNode and build NFA via re.h API ---

static void _emit_re_ast(Re* re, Aut* aut, ReAstNode* node, DebugInfo di) {
  switch (node->kind) {
  case RE_AST_CHAR:
    re_append_ch(re, node->codepoint, di);
    break;
  case RE_AST_RANGE: {
    ReRange* rng = re_range_new();
    re_range_add(rng, node->range_lo, node->range_hi);
    re_append_range(re, rng, di);
    re_range_del(rng);
    break;
  }
  case RE_AST_DOT: {
    ReRange* rng = re_range_new();
    re_range_add(rng, 0, '\n' - 1);
    re_range_add(rng, '\n' + 1, 0x10FFFF);
    re_append_range(re, rng, di);
    re_range_del(rng);
    break;
  }
  case RE_AST_SHORTHAND: {
    ReRange* rng = re_range_new();
    switch (node->shorthand) {
    case 's':
      re_range_add(rng, ' ', ' ');
      re_range_add(rng, '\t', '\t');
      re_range_add(rng, '\n', '\n');
      re_range_add(rng, '\r', '\r');
      break;
    case 'w':
      re_range_add(rng, 'a', 'z');
      re_range_add(rng, 'A', 'Z');
      re_range_add(rng, '0', '9');
      re_range_add(rng, '_', '_');
      break;
    case 'd':
      re_range_add(rng, '0', '9');
      break;
    case 'h':
      re_range_add(rng, '0', '9');
      re_range_add(rng, 'a', 'f');
      re_range_add(rng, 'A', 'F');
      break;
    case 'a':
      re_append_ch(re, LEX_CP_BOF, di);
      re_range_del(rng);
      return;
    case 'z':
      re_append_ch(re, LEX_CP_EOF, di);
      re_range_del(rng);
      return;
    }
    re_append_range(re, rng, di);
    re_range_del(rng);
    break;
  }
  case RE_AST_CHARCLASS: {
    ReRange* rng = re_range_new();
    for (int32_t i = 0; i < (int32_t)darray_size(node->children); i++) {
      ReAstNode* child = &node->children[i];
      if (child->kind == RE_AST_RANGE) {
        re_range_add(rng, child->range_lo, child->range_hi);
      } else if (child->kind == RE_AST_CHAR) {
        re_range_add(rng, child->codepoint, child->codepoint);
      }
    }
    if (node->negated) {
      re_range_neg(rng);
    }
    re_append_range(re, rng, di);
    re_range_del(rng);
    break;
  }
  case RE_AST_SEQ:
    for (int32_t i = 0; i < (int32_t)darray_size(node->children); i++) {
      _emit_re_ast(re, aut, &node->children[i], di);
    }
    break;
  case RE_AST_ALT:
    re_lparen(re);
    for (int32_t i = 0; i < (int32_t)darray_size(node->children); i++) {
      if (i > 0) {
        re_fork(re);
      }
      _emit_re_ast(re, aut, &node->children[i], di);
    }
    re_rparen(re);
    break;
  case RE_AST_GROUP:
    re_lparen(re);
    for (int32_t i = 0; i < (int32_t)darray_size(node->children); i++) {
      _emit_re_ast(re, aut, &node->children[i], di);
    }
    re_rparen(re);
    break;
  case RE_AST_QUANTIFIED: {
    if ((int32_t)darray_size(node->children) < 1) {
      break;
    }
    ReAstNode* inner = &node->children[0];
    int32_t before = re_cur_state(re);
    _emit_re_ast(re, aut, inner, di);
    int32_t after = re_cur_state(re);
    if (node->quantifier == '?') {
      aut_epsilon(aut, before, after);
    } else if (node->quantifier == '+') {
      aut_epsilon(aut, after, before);
    } else if (node->quantifier == '*') {
      aut_epsilon(aut, before, after);
      aut_epsilon(aut, after, before);
    }
    break;
  }
  }
}

// --- Resolve scope body into DFA/state patterns ---

static int32_t _resolve_action(ActionRegistry* reg, ScopeInfo* scope, VpaUnit* unit, const char* default_tok_name,
                               EffectDecl* effects, PegRule* peg_rules, bool allow_empty) {
  const char* tok_name = (unit->name && unit->name[0]) ? unit->name : default_tok_name;
  int32_t tok_id = tok_name ? _register_token(reg, tok_name) : 0;
  bool pop_scope = unit->hook == TOK_HOOK_END || _hook_has_effect(effects, unit->user_hook, TOK_HOOK_END);
  const char* parse_scope_name = (pop_scope && _scope_has_peg(peg_rules, scope->name)) ? scope->name : NULL;
  bool has_user_hook = unit->user_hook && unit->user_hook[0];
  bool needs_action =
      allow_empty || tok_id > 0 || pop_scope || unit->hook != 0 || has_user_hook || parse_scope_name != NULL;

  if (!needs_action) {
    return 0;
  }
  if (tok_id > 0 && !pop_scope && unit->hook == 0 && !has_user_hook && !parse_scope_name) {
    return tok_id;
  }
  return _register_action(reg, tok_id, -1, pop_scope, unit->hook, unit->user_hook, parse_scope_name);
}

static DfaPattern* _resolve_body(ScopeInfo* scope, VpaUnit* body, VpaRule* rules, ScopeInfo* scopes, ActionRegistry* reg,
                                 StateDecl* states, EffectDecl* effects, PegRule* peg_rules) {
  DfaPattern* patterns = darray_new(sizeof(DfaPattern), 0);

  for (int32_t i = 0; i < (int32_t)darray_size(body); i++) {
    VpaUnit* u = &body[i];

    if (u->kind == VPA_REGEXP && u->re_ast) {
      int32_t action_id = _resolve_action(reg, scope, u, NULL, effects, peg_rules, false);
      if (action_id == 0) {
        continue;
      }
      darray_push(patterns, ((DfaPattern){.kind = VPA_REGEXP, .ast = u->re_ast, .state_name = NULL, .action_id = action_id}));

    } else if (u->kind == VPA_STATE && u->state_name && _find_state(states, u->state_name)) {
      int32_t action_id = _resolve_action(reg, scope, u, NULL, effects, peg_rules, true);
      darray_push(patterns,
                  ((DfaPattern){.kind = VPA_STATE, .ast = NULL, .state_name = u->state_name, .action_id = action_id}));

    } else if (u->kind == VPA_REF && u->name) {
      VpaRule* ref_rule = _find_rule(rules, u->name);
      if (!ref_rule) {
        continue;
      }
      VpaUnit* scope_unit = _find_scope_unit(ref_rule);
      if (scope_unit && scope_unit->re_ast) {
        ScopeInfo* target = _find_scope(scopes, ref_rule->name);
        if (target) {
          int32_t aid = _register_action(reg, 0, target->scope_id, false, scope_unit->hook, scope_unit->user_hook, NULL);
          darray_push(patterns,
                      ((DfaPattern){.kind = VPA_REGEXP, .ast = scope_unit->re_ast, .state_name = NULL, .action_id = aid}));
        }
      }
      for (int32_t j = 0; j < (int32_t)darray_size(ref_rule->units); j++) {
        VpaUnit* ru = &ref_rule->units[j];
        if (ru->kind == VPA_REGEXP && ru->re_ast) {
          int32_t aid = _resolve_action(reg, scope, ru, ref_rule->name, effects, peg_rules, false);
          if (aid == 0) {
            continue;
          }
          darray_push(patterns,
                      ((DfaPattern){.kind = VPA_REGEXP, .ast = ru->re_ast, .state_name = NULL, .action_id = aid}));
        } else if (ru->kind == VPA_STATE && ru->state_name && _find_state(states, ru->state_name)) {
          int32_t aid = _resolve_action(reg, scope, ru, ref_rule->name, effects, peg_rules, true);
          darray_push(patterns,
                      ((DfaPattern){.kind = VPA_STATE, .ast = NULL, .state_name = ru->state_name, .action_id = aid}));
        }
      }

    } else if (u->kind == VPA_SCOPE && u->re_ast) {
      ScopeInfo* target = _find_scope(scopes, u->name ? u->name : "");
      if (target) {
        int32_t aid = _register_action(reg, 0, target->scope_id, false, u->hook, u->user_hook, NULL);
        darray_push(patterns,
                    ((DfaPattern){.kind = VPA_REGEXP, .ast = u->re_ast, .state_name = NULL, .action_id = aid}));
      }
    }
  }

  return patterns;
}

// --- Build DFA from resolved patterns ---

static void _gen_scope_dfa(ScopeInfo* scope, DfaPattern* patterns, IrWriter* w) {
  int32_t fn_len = snprintf(NULL, 0, "lex_%s", scope->name) + 1;
  char func_name[fn_len];
  snprintf(func_name, (size_t)fn_len, "lex_%s", scope->name);

  int32_t n_regex = 0;
  for (int32_t i = 0; i < (int32_t)darray_size(patterns); i++) {
    if (patterns[i].kind == VPA_REGEXP && patterns[i].ast) {
      n_regex++;
    }
  }
  if (n_regex == 0) {
    irwriter_rawf(w, "define {i64, i64} @%s(i64 %%state, i64 %%cp) {\n", func_name);
    irwriter_raw(w, "entry:\n");
    irwriter_raw(w, "  ret {i64, i64} {i64 0, i64 -2}\n");
    irwriter_raw(w, "}\n\n");
    return;
  }

  Aut* aut = aut_new(func_name, "nest");
  Re* re = re_new(aut);
  re_lparen(re);

  int32_t emitted = 0;
  for (int32_t i = 0; i < (int32_t)darray_size(patterns); i++) {
    if (patterns[i].kind != VPA_REGEXP || !patterns[i].ast) {
      continue;
    }
    if (emitted > 0) {
      re_fork(re);
    }
    DebugInfo di = {0, 0};
    _emit_re_ast(re, aut, patterns[i].ast, di);
    re_action(re, patterns[i].action_id);
    emitted++;
  }

  re_rparen(re);
  aut_optimize(aut);
  aut_gen_dfa(aut, w, false);

  re_del(re);
  aut_del(aut);
}

static void _gen_scope_state_matcher(ScopeInfo* scope, DfaPattern* patterns, IrWriter* w) {
  int32_t n_states = 0;
  for (int32_t i = 0; i < (int32_t)darray_size(patterns); i++) {
    if (patterns[i].kind == VPA_STATE && patterns[i].state_name && patterns[i].state_name[0]) {
      n_states++;
    }
  }

  int32_t fn_len = snprintf(NULL, 0, "state_%s", scope->name) + 1;
  char func_name[fn_len];
  snprintf(func_name, (size_t)fn_len, "state_%s", scope->name);

  if (n_states == 0) {
    irwriter_rawf(w, "define {i32, i32} @%s(ptr %%src, i32 %%cp_off, ptr %%tt) {\n", func_name);
    irwriter_raw(w, "entry:\n");
    irwriter_raw(w, "  ret {i32, i32} zeroinitializer\n");
    irwriter_raw(w, "}\n\n");
    return;
  }

  irwriter_rawf(w, "define {i32, i32} @%s(ptr %%src, i32 %%cp_off, ptr %%tt) {\n", func_name);
  irwriter_raw(w, "entry:\n");
  irwriter_raw(w, "  %best_act = alloca i32\n");
  irwriter_raw(w, "  %best_off = alloca i32\n");
  irwriter_raw(w, "  store i32 0, ptr %best_act\n");
  irwriter_raw(w, "  store i32 0, ptr %best_off\n");

  for (int32_t i = 0; i < (int32_t)darray_size(patterns); i++) {
    DfaPattern* p = &patterns[i];
    if (p->kind != VPA_STATE || !p->state_name || !p->state_name[0]) {
      continue;
    }

    int32_t sym_len = snprintf(NULL, 0, "match_%s", p->state_name) + 1;
    char symbol[sym_len];
    _make_state_match_symbol(p->state_name, symbol, sizeof(symbol));

    irwriter_rawf(w, "  %%len_%d = call i32 @%s(ptr %%src, i32 %%cp_off, ptr %%tt)\n", i, symbol);
    irwriter_rawf(w, "  %%ok_%d = icmp sgt i32 %%len_%d, 0\n", i, i);
    irwriter_rawf(w, "  br i1 %%ok_%d, label %%Lstate_hit_%d, label %%Lstate_next_%d\n", i, i, i);
    irwriter_rawf(w, "Lstate_hit_%d:\n", i);
    irwriter_rawf(w, "  %%end_%d = add i32 %%cp_off, %%len_%d\n", i, i);
    irwriter_rawf(w, "  %%cur_act_%d = load i32, ptr %%best_act\n", i);
    irwriter_rawf(w, "  %%cur_off_%d = load i32, ptr %%best_off\n", i);
    irwriter_rawf(w, "  %%off_gt_%d = icmp sgt i32 %%end_%d, %%cur_off_%d\n", i, i, i);
    irwriter_rawf(w, "  %%off_eq_%d = icmp eq i32 %%end_%d, %%cur_off_%d\n", i, i, i);
    irwriter_rawf(w, "  %%act_lt_%d = icmp slt i32 %d, %%cur_act_%d\n", i, p->action_id, i);
    irwriter_rawf(w, "  %%cur_missing_%d = icmp eq i32 %%cur_act_%d, 0\n", i, i);
    irwriter_rawf(w, "  %%tie_%d = and i1 %%off_eq_%d, %%act_lt_%d\n", i, i, i);
    irwriter_rawf(w, "  %%replace_a_%d = or i1 %%cur_missing_%d, %%off_gt_%d\n", i, i, i);
    irwriter_rawf(w, "  %%replace_%d = or i1 %%replace_a_%d, %%tie_%d\n", i, i, i);
    irwriter_rawf(w, "  br i1 %%replace_%d, label %%Lstate_store_%d, label %%Lstate_next_%d\n", i, i, i);
    irwriter_rawf(w, "Lstate_store_%d:\n", i);
    irwriter_rawf(w, "  store i32 %d, ptr %%best_act\n", p->action_id);
    irwriter_rawf(w, "  store i32 %%end_%d, ptr %%best_off\n", i);
    irwriter_rawf(w, "  br label %%Lstate_next_%d\n", i);
    irwriter_rawf(w, "Lstate_next_%d:\n", i);
  }

  irwriter_raw(w, "  %ret0 = insertvalue {i32, i32} poison, i32 0, 0\n");
  irwriter_raw(w, "  %final_act = load i32, ptr %best_act\n");
  irwriter_raw(w, "  %ret1 = insertvalue {i32, i32} %ret0, i32 %final_act, 0\n");
  irwriter_raw(w, "  %final_off = load i32, ptr %best_off\n");
  irwriter_raw(w, "  %ret2 = insertvalue {i32, i32} %ret1, i32 %final_off, 1\n");
  irwriter_raw(w, "  ret {i32, i32} %ret2\n");
  irwriter_raw(w, "}\n\n");
}

static void _gen_state_matcher_ir_decls(StateDecl* states, IrWriter* w) {
  for (int32_t i = 0; i < (int32_t)darray_size(states); i++) {
    int32_t sym_len = snprintf(NULL, 0, "match_%s", states[i].name) + 1;
    char symbol[sym_len];
    _make_state_match_symbol(states[i].name, symbol, sizeof(symbol));
    irwriter_declare(w, "i32", symbol, "ptr, i32, ptr");
  }
}

static void _gen_parse_scope_bridges_ir(ActionRegistry* reg, IrWriter* w) {
  bool has_parse = false;
  for (int32_t i = 0; i < (int32_t)darray_size(reg->entries); i++) {
    if (_is_first_parse_scope(reg, i)) {
      has_parse = true;
      break;
    }
  }
  if (!has_parse) {
    return;
  }

  irwriter_declare(w, "i32", "vpa_rt_current_chunk_len", "ptr");
  irwriter_declare(w, "void", "vpa_rt_begin_parse", "ptr");
  irwriter_declare(w, "void", "vpa_rt_end_parse", "ptr");
  irwriter_declare(w, "ptr", "malloc", "i64");
  irwriter_declare(w, "void", "free", "ptr");
  irwriter_raw(w, "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)\n\n");

  for (int32_t i = 0; i < (int32_t)darray_size(reg->entries); i++) {
    if (!_is_first_parse_scope(reg, i)) {
      continue;
    }

    const char* scope_name = reg->entries[i].parse_scope_name;
    int32_t helper_len = snprintf(NULL, 0, "vpa_parse_%s", scope_name) + 1;
    char helper_name[helper_len];
    _make_parse_scope_symbol(scope_name, helper_name, sizeof(helper_name));

    int32_t parse_len = snprintf(NULL, 0, "parse_%s", scope_name) + 1;
    char parse_name[parse_len];
    snprintf(parse_name, sizeof(parse_name), "parse_%s", scope_name);

    int32_t col_type_len = snprintf(NULL, 0, "%%Col.%s", scope_name) + 1;
    char col_type[col_type_len];
    snprintf(col_type, sizeof(col_type), "%%Col.%s", scope_name);

    irwriter_rawf(w, "define internal void @%s(ptr %%tt) {\n", helper_name);
    irwriter_raw(w, "entry:\n");
    irwriter_raw(w, "  %ncols0 = call i32 @vpa_rt_current_chunk_len(ptr %tt)\n");
    irwriter_raw(w, "  %ncols1 = add i32 %ncols0, 1\n");
    irwriter_raw(w, "  %ncols64 = sext i32 %ncols1 to i64\n");
    irwriter_rawf(w, "  %%elt_end = getelementptr %s, ptr null, i32 1\n", col_type);
    irwriter_raw(w, "  %elt_size = ptrtoint ptr %elt_end to i64\n");
    irwriter_raw(w, "  %table_size = mul i64 %ncols64, %elt_size\n");
    irwriter_raw(w, "  call void @vpa_rt_begin_parse(ptr %tt)\n");
    irwriter_raw(w, "  %table = call ptr @malloc(i64 %table_size)\n");
    irwriter_raw(w, "  %table_ok = icmp ne ptr %table, null\n");
    irwriter_raw(w, "  br i1 %table_ok, label %Lparse_init, label %Lparse_done\n\n");
    irwriter_raw(w, "Lparse_init:\n");
    irwriter_raw(w, "  call void @llvm.memset.p0.i64(ptr %table, i8 -1, i64 %table_size, i1 false)\n");
    irwriter_rawf(w, "  %%parse_len = call i32 @%s(ptr %%table, i32 0)\n", parse_name);
    irwriter_raw(w, "  call void @free(ptr %table)\n");
    irwriter_raw(w, "  br label %Lparse_done\n\n");
    irwriter_raw(w, "Lparse_done:\n");
    irwriter_raw(w, "  call void @vpa_rt_end_parse(ptr %tt)\n");
    irwriter_raw(w, "  ret void\n");
    irwriter_raw(w, "}\n\n");
  }
}

// --- Header generation ---

static void _gen_runtime_types(HeaderWriter* hw) {
  hw_blank(hw);
  hw_raw(hw, "#include <stdint.h>\n");
  hw_raw(hw, "#include <stdlib.h>\n");
  hw_raw(hw, "#include <string.h>\n");
  hw_blank(hw);

  hw_comment(hw, "Token (16 bytes)");
  hw_struct_begin(hw, "VpaToken");
  hw_field(hw, "int32_t", "tok_id");
  hw_field(hw, "int32_t", "cp_start");
  hw_field(hw, "int32_t", "cp_size");
  hw_field(hw, "int32_t", "chunk_id");
  hw_struct_end(hw);
  hw_raw(hw, " VpaToken;\n\n");

  hw_comment(hw, "TokenChunk");
  hw_struct_begin(hw, "TokenChunk");
  hw_field(hw, "int32_t", "chunk_id");
  hw_field(hw, "int32_t", "scope_id");
  hw_field(hw, "int32_t", "count");
  hw_field(hw, "int32_t", "capacity");
  hw_field(hw, "VpaToken*", "tokens");
  hw_struct_end(hw);
  hw_raw(hw, " TokenChunk;\n\n");

  hw_comment(hw, "ChunkTable");
  hw_struct_begin(hw, "ChunkTable");
  hw_field(hw, "int32_t", "count");
  hw_field(hw, "int32_t", "capacity");
  hw_field(hw, "TokenChunk*", "chunks");
  hw_struct_end(hw);
  hw_raw(hw, " ChunkTable;\n\n");

  hw_comment(hw, "TokenTree");
  hw_struct_begin(hw, "TokenTree");
  hw_field(hw, "uint64_t*", "newline_map");
  hw_field(hw, "uint64_t*", "token_end_map");
  hw_field(hw, "int32_t", "newline_map_size");
  hw_field(hw, "int32_t", "token_end_map_size");
  hw_field(hw, "ChunkTable", "chunk_table");
  hw_field(hw, "TokenChunk*", "root");
  hw_field(hw, "TokenChunk*", "current");
  hw_field(hw, "TokenChunk*", "parse_chunk");
  hw_field(hw, "int32_t", "scope_stack[64]");
  hw_field(hw, "int32_t", "sp");
  hw_struct_end(hw);
  hw_raw(hw, " TokenTree;\n\n");
}

static void _gen_runtime_helpers(HeaderWriter* hw) {
  hw_blank(hw);
  hw_comment(hw, "TokenTree helpers");

  hw_raw(hw, "static inline TokenChunk* tt_alloc_chunk(TokenTree* tt, int32_t scope_id) {\n"
             "  ChunkTable* ct = &tt->chunk_table;\n"
             "  if (ct->count >= ct->capacity) {\n"
             "    ct->capacity = ct->capacity ? ct->capacity * 2 : 16;\n"
             "    ct->chunks = (TokenChunk*)realloc(ct->chunks, sizeof(TokenChunk) * ct->capacity);\n"
             "  }\n"
             "  TokenChunk* c = &ct->chunks[ct->count];\n"
             "  c->chunk_id = ct->count++;\n"
             "  c->scope_id = scope_id;\n"
             "  c->count = 0;\n"
             "  c->capacity = 32;\n"
             "  c->tokens = (VpaToken*)malloc(sizeof(VpaToken) * 32);\n"
             "  return c;\n"
             "}\n\n");

  hw_raw(hw, "static inline void tt_add_token(TokenChunk* chunk, int32_t tok_id,\n"
             "                                int32_t cp_start, int32_t cp_size) {\n"
             "  if (chunk->count >= chunk->capacity) {\n"
             "    chunk->capacity *= 2;\n"
             "    chunk->tokens = (VpaToken*)realloc(chunk->tokens, sizeof(VpaToken) * chunk->capacity);\n"
             "  }\n"
             "  chunk->tokens[chunk->count++] = (VpaToken){tok_id, cp_start, cp_size, -1};\n"
             "}\n\n");

  hw_raw(hw, "static inline TokenTree* tt_new(int32_t cp_count) {\n"
             "  TokenTree* tt = (TokenTree*)calloc(1, sizeof(TokenTree));\n"
             "  int32_t map_words = (cp_count + 63) / 64;\n"
             "  tt->newline_map_size = map_words;\n"
             "  tt->token_end_map_size = map_words;\n"
             "  tt->newline_map = (uint64_t*)calloc(map_words, sizeof(uint64_t));\n"
             "  tt->token_end_map = (uint64_t*)calloc(map_words, sizeof(uint64_t));\n"
             "  tt->root = tt_alloc_chunk(tt, 0);\n"
             "  tt->current = tt->root;\n"
             "  tt->parse_chunk = NULL;\n"
             "  tt->sp = 0;\n"
             "  tt->scope_stack[0] = 0;\n"
             "  return tt;\n"
             "}\n\n");

  hw_raw(hw, "static inline void tt_del(TokenTree* tt) {\n"
             "  if (!tt) return;\n"
             "  for (int32_t i = 0; i < tt->chunk_table.count; i++) {\n"
             "    free(tt->chunk_table.chunks[i].tokens);\n"
             "  }\n"
             "  free(tt->chunk_table.chunks);\n"
             "  free(tt->newline_map);\n"
             "  free(tt->token_end_map);\n"
             "  free(tt);\n"
             "}\n\n");

  hw_raw(hw, "static inline void tt_mark_newline(TokenTree* tt, int32_t cp_off) {\n"
             "  tt->newline_map[cp_off / 64] |= (uint64_t)1 << (cp_off % 64);\n"
             "}\n\n");

  hw_raw(hw, "static inline void tt_mark_token_end(TokenTree* tt, int32_t cp_off) {\n"
             "  tt->token_end_map[cp_off / 64] |= (uint64_t)1 << (cp_off % 64);\n"
             "}\n\n");

  hw_comment(hw, "Runtime hooks");
  hw_raw(hw, "int32_t vpa_rt_read_cp(void* src, int32_t cp_off);\n");
  hw_raw(hw, "\n");
  hw_raw(hw, "#ifndef VPA_IMPLEMENTATION\n");
  hw_raw(hw, "void vpa_rt_emit_token(void* tt, int32_t tok_id, int32_t cp_start, int32_t cp_size);\n");
  hw_raw(hw, "void vpa_rt_push_scope(void* tt, int32_t scope_id);\n");
  hw_raw(hw, "void vpa_rt_pop_scope(void* tt);\n");
  hw_raw(hw, "int32_t vpa_rt_get_scope(void* tt);\n");
  hw_raw(hw, "int32_t vpa_rt_current_chunk_len(void* tt);\n");
  hw_raw(hw, "void vpa_rt_begin_parse(void* tt);\n");
  hw_raw(hw, "void vpa_rt_end_parse(void* tt);\n");
  hw_raw(hw, "int32_t match_tok(int32_t tok_id, int32_t col);\n");
  hw_raw(hw, "#else\n");
  hw_raw(hw, "static TokenTree* _vpa_parse_tt = NULL;\n\n");
  hw_raw(hw, "void vpa_rt_emit_token(void* tt, int32_t tok_id, int32_t cp_start, int32_t cp_size) {\n"
             "  TokenTree* tree = (TokenTree*)tt;\n"
             "  if (!tree || !tree->current) return;\n"
             "  tt_add_token(tree->current, tok_id, cp_start, cp_size);\n"
             "  if (cp_size > 0) {\n"
             "    tt_mark_token_end(tree, cp_start + cp_size - 1);\n"
             "  }\n"
             "}\n\n");
  hw_raw(hw, "void vpa_rt_push_scope(void* tt, int32_t scope_id) {\n"
             "  TokenTree* tree = (TokenTree*)tt;\n"
             "  if (!tree || tree->sp >= 63) return;\n"
             "  TokenChunk* child = tt_alloc_chunk(tree, scope_id);\n"
             "  tree->scope_stack[++tree->sp] = child->chunk_id;\n"
             "  tree->current = child;\n"
             "}\n\n");
  hw_raw(hw, "void vpa_rt_pop_scope(void* tt) {\n"
             "  TokenTree* tree = (TokenTree*)tt;\n"
             "  if (!tree || tree->sp <= 0) return;\n"
             "  tree->sp--;\n"
             "  tree->current = &tree->chunk_table.chunks[tree->scope_stack[tree->sp]];\n"
             "}\n\n");
  hw_raw(hw, "int32_t vpa_rt_get_scope(void* tt) {\n"
             "  TokenTree* tree = (TokenTree*)tt;\n"
             "  return (tree && tree->current) ? tree->current->scope_id : 0;\n"
             "}\n\n");
  hw_raw(hw, "int32_t vpa_rt_current_chunk_len(void* tt) {\n"
             "  TokenTree* tree = (TokenTree*)tt;\n"
             "  return (tree && tree->current) ? tree->current->count : 0;\n"
             "}\n\n");
  hw_raw(hw, "void vpa_rt_begin_parse(void* tt) {\n"
             "  TokenTree* tree = (TokenTree*)tt;\n"
             "  _vpa_parse_tt = tree;\n"
             "  if (tree) {\n"
             "    tree->parse_chunk = tree->current;\n"
             "  }\n"
             "}\n\n");
  hw_raw(hw, "void vpa_rt_end_parse(void* tt) {\n"
             "  TokenTree* tree = (TokenTree*)tt;\n"
             "  if (tree) {\n"
             "    tree->parse_chunk = NULL;\n"
             "  }\n"
             "  if (_vpa_parse_tt == tree) {\n"
             "    _vpa_parse_tt = NULL;\n"
             "  }\n"
             "}\n\n");
  hw_raw(hw, "int32_t match_tok(int32_t tok_id, int32_t col) {\n"
             "  if (!_vpa_parse_tt || !_vpa_parse_tt->parse_chunk) return -1;\n"
             "  if (col < 0 || col >= _vpa_parse_tt->parse_chunk->count) return -1;\n"
             "  return _vpa_parse_tt->parse_chunk->tokens[col].tok_id == tok_id ? 1 : -1;\n"
             "}\n");
  hw_raw(hw, "#endif\n\n");
}

static void _gen_state_matcher_header(StateDecl* states, HeaderWriter* hw) {
  if ((int32_t)darray_size(states) == 0) {
    return;
  }

  hw_comment(hw, "State matchers: return matched codepoint length, or 0 for no match");
  for (int32_t i = 0; i < (int32_t)darray_size(states); i++) {
    hw_fmt(hw, "int32_t match_%s(void* src, int32_t cp_off, void* tt);\n", states[i].name);
  }
  hw_blank(hw);
}

static void _gen_user_hook_header(ActionRegistry* reg, HeaderWriter* hw) {
  bool has_hooks = false;
  for (int32_t i = 0; i < (int32_t)darray_size(reg->entries); i++) {
    if (_is_first_user_hook(reg, i)) {
      has_hooks = true;
      break;
    }
  }
  if (!has_hooks) {
    return;
  }

  hw_comment(hw, "User hook callbacks (.foo -> vpa_hook_foo)");
  for (int32_t i = 0; i < (int32_t)darray_size(reg->entries); i++) {
    if (!_is_first_user_hook(reg, i)) {
      continue;
    }
    int32_t sym_len = snprintf(NULL, 0, "vpa_hook_%s", reg->entries[i].user_hook + 1) + 1;
    char symbol[sym_len];
    _make_user_hook_symbol(reg->entries[i].user_hook, symbol, sizeof(symbol));
    hw_fmt(hw, "void %s(void* tt, int32_t cp_start, int32_t cp_size);\n", symbol);
  }
  hw_blank(hw);
}

static void _gen_token_header(ActionRegistry* reg, HeaderWriter* hw) {
  hw_blank(hw);
  hw_comment(hw, "Token IDs");
  int32_t count = (int32_t)darray_size(reg->entries);
  for (int32_t i = 0; i < count; i++) {
    if (!reg->tok_names[i]) {
      continue;
    }
    int32_t dn_len = snprintf(NULL, 0, "TOK_%s", reg->tok_names[i]) + 1;
    char define_name[dn_len];
    snprintf(define_name, (size_t)dn_len, "TOK_%s", reg->tok_names[i]);
    for (char* p = define_name + 4; *p; p++) {
      if (*p >= 'a' && *p <= 'z') {
        *p -= 32;
      } else if (*p == '.') {
        *p = '_';
      }
    }
    hw_define(hw, define_name, reg->entries[i].tok_id);
  }
}

static void _gen_scope_ids(ScopeInfo* scopes, HeaderWriter* hw) {
  hw_blank(hw);
  hw_comment(hw, "Scope IDs");
  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    int32_t dn_len = snprintf(NULL, 0, "SCOPE_%s", scopes[i].name) + 1;
    char define_name[dn_len];
    snprintf(define_name, (size_t)dn_len, "SCOPE_%s", scopes[i].name);
    for (char* p = define_name + 6; *p; p++) {
      if (*p >= 'a' && *p <= 'z') {
        *p -= 32;
      }
    }
    hw_define(hw, define_name, scopes[i].scope_id);
  }
}

static void _gen_lex_declarations(ScopeInfo* scopes, HeaderWriter* hw) {
  hw_blank(hw);
  hw_comment(hw, "Lexer declarations");
  hw_raw(hw, "typedef struct { int64_t state; int64_t action; } LexResult;\n");
  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    hw_fmt(hw, "extern LexResult lex_%s(int64_t state, int64_t cp);\n", scopes[i].name);
  }
  hw_raw(hw, "extern void vpa_lex(int64_t src, int64_t len, int64_t tt);\n");
}

static void _gen_action_table_header(ActionRegistry* reg, ScopeInfo* scopes, HeaderWriter* hw) {
  hw_blank(hw);
  hw_fmt(hw, "#define VPA_N_ACTIONS %d\n", (int32_t)darray_size(reg->entries));
  hw_fmt(hw, "#define VPA_N_SCOPES %d\n", (int32_t)darray_size(scopes));
}

// --- IR: action dispatch function ---
// define void @vpa_dispatch(ptr %tt, i32 %action_id, i32 %cp_start, i32 %cp_size)
// switch on action_id -> per-action label blocks with micro-ops

static void _gen_action_dispatch_ir(ActionRegistry* reg, IrWriter* w) {
  irwriter_declare(w, "void", "vpa_rt_emit_token", "ptr, i32, i32, i32");
  irwriter_declare(w, "void", "vpa_rt_push_scope", "ptr, i32");
  irwriter_declare(w, "void", "vpa_rt_pop_scope", "ptr");
  for (int32_t i = 0; i < (int32_t)darray_size(reg->entries); i++) {
    if (!_is_first_user_hook(reg, i)) {
      continue;
    }
    int32_t sym_len = snprintf(NULL, 0, "vpa_hook_%s", reg->entries[i].user_hook + 1) + 1;
    char symbol[sym_len];
    _make_user_hook_symbol(reg->entries[i].user_hook, symbol, sizeof(symbol));
    irwriter_declare(w, "void", symbol, "ptr, i32, i32");
  }
  int32_t n = (int32_t)darray_size(reg->entries);

  irwriter_raw(w, "define void @vpa_dispatch(ptr %tt, i32 %action_id, i32 %cp_start, i32 %cp_size) {\n");
  irwriter_raw(w, "entry:\n");
  irwriter_raw(w, "  switch i32 %action_id, label %Ldefault [\n");
  for (int32_t i = 0; i < n; i++) {
    irwriter_rawf(w, "    i32 %d, label %%Lact_%d\n", i + 1, i + 1);
  }
  irwriter_raw(w, "  ]\n");

  irwriter_raw(w, "Ldefault:\n  ret void\n");

  for (int32_t i = 0; i < n; i++) {
    ActionEntry* e = &reg->entries[i];
    irwriter_rawf(w, "Lact_%d:\n", i + 1);
    if (_has_user_hook(e)) {
      int32_t sym_len = snprintf(NULL, 0, "vpa_hook_%s", e->user_hook + 1) + 1;
      char symbol[sym_len];
      _make_user_hook_symbol(e->user_hook, symbol, sizeof(symbol));
      irwriter_rawf(w, "  call void @%s(ptr %%tt, i32 %%cp_start, i32 %%cp_size)\n", symbol);
    }
    if (e->tok_id > 0) {
      irwriter_rawf(w, "  call void @vpa_rt_emit_token(ptr %%tt, i32 %d, i32 %%cp_start, i32 %%cp_size)\n", e->tok_id);
    }
    if (e->push_scope_id >= 0) {
      irwriter_rawf(w, "  call void @vpa_rt_push_scope(ptr %%tt, i32 %d)\n", e->push_scope_id);
    }
    if (e->pop_scope) {
      if (_has_parse_scope(e)) {
        int32_t sym_len = snprintf(NULL, 0, "vpa_parse_%s", e->parse_scope_name) + 1;
        char symbol[sym_len];
        _make_parse_scope_symbol(e->parse_scope_name, symbol, sizeof(symbol));
        irwriter_rawf(w, "  call void @%s(ptr %%tt)\n", symbol);
      }
      irwriter_raw(w, "  call void @vpa_rt_pop_scope(ptr %tt)\n");
    }
    irwriter_raw(w, "  ret void\n");
  }

  irwriter_raw(w, "}\n\n");
}

// --- IR: outer lexing loop ---
// Longest-match: feed codepoints until DFA rejects, then dispatch the last accepting action.
//
// define void @vpa_lex(ptr %src, i32 %len, ptr %tt)
//
// Uses allocas for all mutable state. LLVM mem2reg promotes to SSA.

static void _gen_lex_loop_ir(ScopeInfo* scopes, IrWriter* w) {
  irwriter_declare(w, "i32", "vpa_rt_read_cp", "ptr, i32");
  irwriter_declare(w, "i32", "vpa_rt_get_scope", "ptr");

  int32_t n = (int32_t)darray_size(scopes);

  irwriter_raw(w, "define void @vpa_lex(ptr %src, i32 %len, ptr %tt) {\n");
  irwriter_raw(w, "entry:\n");

  // Mutable state via alloca
  irwriter_raw(w, "  %cp_off = alloca i32\n");
  irwriter_raw(w, "  %state = alloca i32\n");
  irwriter_raw(w, "  %tok_start = alloca i32\n");
  irwriter_raw(w, "  %last_act = alloca i32\n");
  irwriter_raw(w, "  %last_off = alloca i32\n");
  irwriter_raw(w, "  %new_state = alloca i32\n");
  irwriter_raw(w, "  %act_id = alloca i32\n");
  irwriter_raw(w, "  %state_act = alloca i32\n");
  irwriter_raw(w, "  %state_off = alloca i32\n");
  irwriter_raw(w, "  %state_ready = alloca i32\n");
  irwriter_raw(w, "  %cand_act = alloca i32\n");
  irwriter_raw(w, "  %cand_off = alloca i32\n");

  irwriter_raw(w, "  store i32 0, ptr %cp_off\n");
  irwriter_raw(w, "  store i32 0, ptr %state\n");
  irwriter_raw(w, "  store i32 0, ptr %tok_start\n");
  irwriter_raw(w, "  store i32 0, ptr %last_act\n");
  irwriter_raw(w, "  store i32 0, ptr %last_off\n");
  irwriter_raw(w, "  store i32 0, ptr %state_act\n");
  irwriter_raw(w, "  store i32 0, ptr %state_off\n");
  irwriter_raw(w, "  store i32 0, ptr %state_ready\n");
  irwriter_raw(w, "  br label %Lloop\n\n");

  // --- Lloop: check if we reached the end ---
  irwriter_raw(w, "Lloop:\n");
  irwriter_raw(w, "  %off.0 = load i32, ptr %cp_off\n");
  irwriter_raw(w, "  %at_end = icmp sge i32 %off.0, %len\n");
  irwriter_raw(w, "  br i1 %at_end, label %Lflush, label %Lstate_check\n\n");

  irwriter_raw(w, "Lstate_check:\n");
  irwriter_raw(w, "  %state_ready_val = load i32, ptr %state_ready\n");
  irwriter_raw(w, "  %need_state = icmp eq i32 %state_ready_val, 0\n");
  irwriter_raw(w, "  br i1 %need_state, label %Lstate_eval, label %Lfeed\n\n");

  irwriter_raw(w, "Lstate_eval:\n");
  irwriter_raw(w, "  %scope_sm = call i32 @vpa_rt_get_scope(ptr %tt)\n");
  irwriter_raw(w, "  %sm_off_in = load i32, ptr %cp_off\n");
  irwriter_raw(w, "  switch i32 %scope_sm, label %Lstate_done [\n");
  for (int32_t i = 0; i < n; i++) {
    irwriter_rawf(w, "    i32 %d, label %%Lstate_call_%d\n", scopes[i].scope_id, scopes[i].scope_id);
  }
  irwriter_raw(w, "  ]\n\n");

  for (int32_t i = 0; i < n; i++) {
    int32_t sid = scopes[i].scope_id;
    irwriter_rawf(w, "Lstate_call_%d:\n", sid);
    irwriter_rawf(w, "  %%sm_res_%d = call {i32, i32} @state_%s(ptr %%src, i32 %%sm_off_in, ptr %%tt)\n", sid,
                  scopes[i].name);
    irwriter_rawf(w, "  %%sm_act_%d = extractvalue {i32, i32} %%sm_res_%d, 0\n", sid, sid);
    irwriter_rawf(w, "  %%sm_off_%d = extractvalue {i32, i32} %%sm_res_%d, 1\n", sid, sid);
    irwriter_rawf(w, "  store i32 %%sm_act_%d, ptr %%state_act\n", sid);
    irwriter_rawf(w, "  store i32 %%sm_off_%d, ptr %%state_off\n", sid);
    irwriter_raw(w, "  br label %Lstate_done\n\n");
  }

  irwriter_raw(w, "Lstate_done:\n");
  irwriter_raw(w, "  store i32 1, ptr %state_ready\n");
  irwriter_raw(w, "  br label %Lfeed\n\n");

  // --- Lfeed: read codepoint, call DFA for current scope ---
  irwriter_raw(w, "Lfeed:\n");
  irwriter_raw(w, "  %off.1 = load i32, ptr %cp_off\n");
  irwriter_raw(w, "  %cp = call i32 @vpa_rt_read_cp(ptr %src, i32 %off.1)\n");
  irwriter_raw(w, "  %scope = call i32 @vpa_rt_get_scope(ptr %tt)\n");
  irwriter_raw(w, "  %st = load i32, ptr %state\n");
  irwriter_raw(w, "  %st64 = sext i32 %st to i64\n");
  irwriter_raw(w, "  %cp64 = sext i32 %cp to i64\n");

  // Switch on scope to call the right lex_ function
  irwriter_raw(w, "  switch i32 %scope, label %Ldone [\n");
  for (int32_t i = 0; i < n; i++) {
    irwriter_rawf(w, "    i32 %d, label %%Lcall_%d\n", scopes[i].scope_id, scopes[i].scope_id);
  }
  irwriter_raw(w, "  ]\n\n");

  // Per-scope call blocks: call lex_<name>, extract results, store into allocas, jump to Lresult
  for (int32_t i = 0; i < n; i++) {
    int32_t sid = scopes[i].scope_id;
    irwriter_rawf(w, "Lcall_%d:\n", sid);
    irwriter_rawf(w, "  %%res_%d = call {i64, i64} @lex_%s(i64 %%st64, i64 %%cp64)\n", sid, scopes[i].name);
    irwriter_rawf(w, "  %%ns64_%d = extractvalue {i64, i64} %%res_%d, 0\n", sid, sid);
    irwriter_rawf(w, "  %%ai64_%d = extractvalue {i64, i64} %%res_%d, 1\n", sid, sid);
    irwriter_rawf(w, "  %%ns32_%d = trunc i64 %%ns64_%d to i32\n", sid, sid);
    irwriter_rawf(w, "  %%ai32_%d = trunc i64 %%ai64_%d to i32\n", sid, sid);
    irwriter_rawf(w, "  store i32 %%ns32_%d, ptr %%new_state\n", sid);
    irwriter_rawf(w, "  store i32 %%ai32_%d, ptr %%act_id\n", sid);
    irwriter_raw(w, "  br label %Lresult\n\n");
  }

  // --- Lresult: check DFA output ---
  // action_id > 0 → we have an accepting transition, record it
  // action_id == 0 → valid transition but no action yet, keep going
  // action_id == -2 → DFA rejected, flush last accept
  irwriter_raw(w, "Lresult:\n");
  irwriter_raw(w, "  %ns = load i32, ptr %new_state\n");
  irwriter_raw(w, "  %ai = load i32, ptr %act_id\n");
  irwriter_raw(w, "  %is_reject = icmp eq i32 %ai, -2\n");
  irwriter_raw(w, "  br i1 %is_reject, label %Lreject, label %Laccept_check\n\n");

  // --- Laccept_check: action_id > 0 means accepting state ---
  irwriter_raw(w, "Laccept_check:\n");
  irwriter_raw(w, "  %is_action = icmp sgt i32 %ai, 0\n");
  irwriter_raw(w, "  br i1 %is_action, label %Lrecord, label %Ladvance\n\n");

  // --- Lrecord: record last accepting position ---
  irwriter_raw(w, "Lrecord:\n");
  irwriter_raw(w, "  store i32 %ai, ptr %last_act\n");
  irwriter_raw(w, "  %off.2 = load i32, ptr %cp_off\n");
  irwriter_raw(w, "  %off.3 = add i32 %off.2, 1\n");
  irwriter_raw(w, "  store i32 %off.3, ptr %last_off\n");
  irwriter_raw(w, "  br label %Ladvance\n\n");

  // --- Ladvance: move to next codepoint ---
  irwriter_raw(w, "Ladvance:\n");
  irwriter_raw(w, "  store i32 %ns, ptr %state\n");
  irwriter_raw(w, "  %off.4 = load i32, ptr %cp_off\n");
  irwriter_raw(w, "  %off.5 = add i32 %off.4, 1\n");
  irwriter_raw(w, "  store i32 %off.5, ptr %cp_off\n");
  irwriter_raw(w, "  br label %Lloop\n\n");

  // --- Lreject: DFA rejected, dispatch last accept ---
  irwriter_raw(w, "Lreject:\n");
  irwriter_raw(w, "  %la = load i32, ptr %last_act\n");
  irwriter_raw(w, "  %lo = load i32, ptr %last_off\n");
  irwriter_raw(w, "  store i32 %la, ptr %cand_act\n");
  irwriter_raw(w, "  store i32 %lo, ptr %cand_off\n");
  irwriter_raw(w, "  %sa = load i32, ptr %state_act\n");
  irwriter_raw(w, "  %so = load i32, ptr %state_off\n");
  irwriter_raw(w, "  %state_has = icmp sgt i32 %sa, 0\n");
  irwriter_raw(w, "  br i1 %state_has, label %Lreject_cmp, label %Lreject_after_cmp\n\n");

  irwriter_raw(w, "Lreject_cmp:\n");
  irwriter_raw(w, "  %la_empty = icmp eq i32 %la, 0\n");
  irwriter_raw(w, "  %so_gt = icmp sgt i32 %so, %lo\n");
  irwriter_raw(w, "  %so_eq = icmp eq i32 %so, %lo\n");
  irwriter_raw(w, "  %sa_lt = icmp slt i32 %sa, %la\n");
  irwriter_raw(w, "  %reject_tie = and i1 %so_eq, %sa_lt\n");
  irwriter_raw(w, "  %reject_cmp_a = or i1 %la_empty, %so_gt\n");
  irwriter_raw(w, "  %reject_use_state = or i1 %reject_cmp_a, %reject_tie\n");
  irwriter_raw(w, "  br i1 %reject_use_state, label %Lreject_take_state, label %Lreject_after_cmp\n\n");

  irwriter_raw(w, "Lreject_take_state:\n");
  irwriter_raw(w, "  store i32 %sa, ptr %cand_act\n");
  irwriter_raw(w, "  store i32 %so, ptr %cand_off\n");
  irwriter_raw(w, "  br label %Lreject_after_cmp\n\n");

  irwriter_raw(w, "Lreject_after_cmp:\n");
  irwriter_raw(w, "  %ca = load i32, ptr %cand_act\n");
  irwriter_raw(w, "  %has_accept = icmp sgt i32 %ca, 0\n");
  irwriter_raw(w, "  br i1 %has_accept, label %Ldispatch, label %Lskip\n\n");

  // --- Ldispatch: call vpa_dispatch, reset DFA, continue ---
  irwriter_raw(w, "Ldispatch:\n");
  irwriter_raw(w, "  %ts = load i32, ptr %tok_start\n");
  irwriter_raw(w, "  %co = load i32, ptr %cand_off\n");
  irwriter_raw(w, "  %sz = sub i32 %co, %ts\n");
  irwriter_raw(w, "  call void @vpa_dispatch(ptr %tt, i32 %ca, i32 %ts, i32 %sz)\n");
  // Reset: cp_off = last_off, state = 0, tok_start = last_off, clear last_act
  irwriter_raw(w, "  store i32 %co, ptr %cp_off\n");
  irwriter_raw(w, "  store i32 0, ptr %state\n");
  irwriter_raw(w, "  store i32 %co, ptr %tok_start\n");
  irwriter_raw(w, "  store i32 0, ptr %last_act\n");
  irwriter_raw(w, "  store i32 0, ptr %last_off\n");
  irwriter_raw(w, "  store i32 0, ptr %state_act\n");
  irwriter_raw(w, "  store i32 0, ptr %state_off\n");
  irwriter_raw(w, "  store i32 0, ptr %state_ready\n");
  irwriter_raw(w, "  br label %Lloop\n\n");

  // --- Lskip: no accepting state, skip one codepoint ---
  irwriter_raw(w, "Lskip:\n");
  irwriter_raw(w, "  %off.6 = load i32, ptr %tok_start\n");
  irwriter_raw(w, "  %off.7 = add i32 %off.6, 1\n");
  irwriter_raw(w, "  store i32 %off.7, ptr %cp_off\n");
  irwriter_raw(w, "  store i32 %off.7, ptr %tok_start\n");
  irwriter_raw(w, "  store i32 0, ptr %state\n");
  irwriter_raw(w, "  store i32 0, ptr %last_act\n");
  irwriter_raw(w, "  store i32 0, ptr %last_off\n");
  irwriter_raw(w, "  store i32 0, ptr %state_act\n");
  irwriter_raw(w, "  store i32 0, ptr %state_off\n");
  irwriter_raw(w, "  store i32 0, ptr %state_ready\n");
  irwriter_raw(w, "  br label %Lloop\n\n");

  // --- Lflush: end of input, dispatch any pending accept ---
  irwriter_raw(w, "Lflush:\n");
  irwriter_raw(w, "  %la2 = load i32, ptr %last_act\n");
  irwriter_raw(w, "  %lo2 = load i32, ptr %last_off\n");
  irwriter_raw(w, "  store i32 %la2, ptr %cand_act\n");
  irwriter_raw(w, "  store i32 %lo2, ptr %cand_off\n");
  irwriter_raw(w, "  %sa2 = load i32, ptr %state_act\n");
  irwriter_raw(w, "  %so2 = load i32, ptr %state_off\n");
  irwriter_raw(w, "  %state_has2 = icmp sgt i32 %sa2, 0\n");
  irwriter_raw(w, "  br i1 %state_has2, label %Lflush_cmp, label %Lflush_after_cmp\n\n");

  irwriter_raw(w, "Lflush_cmp:\n");
  irwriter_raw(w, "  %la2_empty = icmp eq i32 %la2, 0\n");
  irwriter_raw(w, "  %so2_gt = icmp sgt i32 %so2, %lo2\n");
  irwriter_raw(w, "  %so2_eq = icmp eq i32 %so2, %lo2\n");
  irwriter_raw(w, "  %sa2_lt = icmp slt i32 %sa2, %la2\n");
  irwriter_raw(w, "  %flush_tie = and i1 %so2_eq, %sa2_lt\n");
  irwriter_raw(w, "  %flush_cmp_a = or i1 %la2_empty, %so2_gt\n");
  irwriter_raw(w, "  %flush_use_state = or i1 %flush_cmp_a, %flush_tie\n");
  irwriter_raw(w, "  br i1 %flush_use_state, label %Lflush_take_state, label %Lflush_after_cmp\n\n");

  irwriter_raw(w, "Lflush_take_state:\n");
  irwriter_raw(w, "  store i32 %sa2, ptr %cand_act\n");
  irwriter_raw(w, "  store i32 %so2, ptr %cand_off\n");
  irwriter_raw(w, "  br label %Lflush_after_cmp\n\n");

  irwriter_raw(w, "Lflush_after_cmp:\n");
  irwriter_raw(w, "  %ca2 = load i32, ptr %cand_act\n");
  irwriter_raw(w, "  %has2 = icmp sgt i32 %ca2, 0\n");
  irwriter_raw(w, "  br i1 %has2, label %Lflush_dispatch, label %Ldone\n\n");

  irwriter_raw(w, "Lflush_dispatch:\n");
  irwriter_raw(w, "  %ts2 = load i32, ptr %tok_start\n");
  irwriter_raw(w, "  %co2 = load i32, ptr %cand_off\n");
  irwriter_raw(w, "  %sz2 = sub i32 %co2, %ts2\n");
  irwriter_raw(w, "  call void @vpa_dispatch(ptr %tt, i32 %ca2, i32 %ts2, i32 %sz2)\n");
  irwriter_raw(w, "  br label %Ldone\n\n");

  irwriter_raw(w, "Ldone:\n");
  irwriter_raw(w, "  ret void\n");
  irwriter_raw(w, "}\n\n");
}

// --- Scope collection ---

static ScopeInfo* _collect_scopes(VpaRule* rules) {
  ScopeInfo* scopes = darray_new(sizeof(ScopeInfo), 0);

  for (int32_t i = 0; i < (int32_t)darray_size(rules); i++) {
    VpaRule* rule = &rules[i];
    if (rule->is_macro) {
      continue;
    }

    if (rule->is_scope) {
      // Bare braced scope: name = { body }
      ScopeInfo s = {
          .name = strdup(rule->name),
          .scope_id = (int32_t)darray_size(scopes),
          .body = rule->units,
          .leader_ast = NULL,
          .leader_hook = 0,
          .leader_user_hook = NULL,
      };
      darray_push(scopes, s);
    } else {
      // Check if any unit is VPA_SCOPE (leader + body)
      VpaUnit* su = _find_scope_unit(rule);
      if (su) {
        ScopeInfo s = {
            .name = strdup(rule->name),
            .scope_id = (int32_t)darray_size(scopes),
            .body = su->children,
            .leader_ast = su->re_ast,
            .leader_hook = su->hook,
            .leader_user_hook = su->user_hook ? strdup(su->user_hook) : NULL,
        };
        darray_push(scopes, s);
      }
    }
  }

  return scopes;
}

// --- Public API ---

void vpa_gen(VpaGenInput* input, HeaderWriter* hw, IrWriter* w) {
  VpaRule* rules = input->rules;
  KeywordEntry* keywords = input->keywords;
  StateDecl* states = input->states;
  EffectDecl* effects = input->effects;
  PegRule* peg_rules = input->peg_rules;

  ActionRegistry reg = {0};
  reg.entries = darray_new(sizeof(ActionEntry), 0);
  reg.tok_names = darray_new(sizeof(char*), 0);

  for (int32_t i = 0; i < (int32_t)darray_size(keywords); i++) {
    int32_t len = keywords[i].lit_len;
    char lit[len + 1];
    memcpy(lit, keywords[i].src + keywords[i].lit_off, (size_t)len);
    lit[len] = '\0';
    int32_t tn_len = snprintf(NULL, 0, "%s.%s", keywords[i].group, lit) + 1;
    char tok_name[tn_len];
    snprintf(tok_name, (size_t)tn_len, "%s.%s", keywords[i].group, lit);
    _register_token(&reg, tok_name);
  }

  ScopeInfo* scopes = _collect_scopes(rules);
  _gen_state_matcher_ir_decls(states, w);

  _gen_runtime_types(hw);
  _gen_runtime_helpers(hw);
  _gen_state_matcher_header(states, hw);
  _gen_scope_ids(scopes, hw);

  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    if (!scopes[i].body) {
      continue;
    }
    DfaPattern* patterns = _resolve_body(&scopes[i], scopes[i].body, rules, scopes, &reg, states, effects, peg_rules);
    _gen_scope_dfa(&scopes[i], patterns, w);
    _gen_scope_state_matcher(&scopes[i], patterns, w);
    darray_del(patterns);
  }

  // Emit parse bridges, action dispatch, and lex loop in IR
  _gen_parse_scope_bridges_ir(&reg, w);
  _gen_action_dispatch_ir(&reg, w);
  _gen_lex_loop_ir(scopes, w);

  // Emit header: declarations, token IDs, action metadata
  _gen_lex_declarations(scopes, hw);
  _gen_user_hook_header(&reg, hw);
  _gen_token_header(&reg, hw);
  _gen_action_table_header(&reg, scopes, hw);

  // Cleanup
  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    free(scopes[i].name);
    free(scopes[i].leader_user_hook);
  }
  darray_del(scopes);
  _free_action_registry(&reg);
}
