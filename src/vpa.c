#include "vpa.h"
#include "aut.h"
#include "darray.h"
#include "parse.h"
#include "re.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../build/nest_rt.inc"

// --- Action registry ---

typedef struct {
  int32_t tok_id;        // 0 when no token should be emitted
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

static int32_t _register_action(ActionRegistry* reg, int32_t tok_id, int32_t push_scope_id, bool pop_scope,
                                int32_t hook, const char* user_hook, const char* parse_scope_name) {
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
  VpaUnit* body;       // NOT owned
  VpaUnit* rule_units; // NOT owned, full rule units (leader regex + hooks accessible here)
} ScopeInfo;

// --- DFA pattern ---

typedef struct {
  VpaUnitKind kind;
  ReIr ir;
  const char* state_name;
  int32_t action_id;
} DfaPattern;

// --- Helpers ---

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
  const char* hook_name = user_hook[0] == '.' ? user_hook + 1 : user_hook;
  snprintf(out, out_sz, "vpa_hook_%s", hook_name);
}

// --- Resolve scope body into DFA/state patterns ---

static int32_t _resolve_action(ActionRegistry* reg, ScopeInfo* scope, VpaUnit* unit, const char* default_tok_name,
                               EffectDecl* effects, PegRule* peg_rules, bool allow_empty) {
  const char* tok_name = (unit->name && unit->name[0]) ? unit->name : default_tok_name;
  int32_t tok_id = tok_name ? _register_token(reg, tok_name) : 0;
  bool pop_scope = unit->hook == TOK_HOOK_END || _hook_has_effect(effects, unit->user_hook, TOK_HOOK_END);
  const char* parse_scope_name = NULL;
  if (pop_scope) {
    for (int32_t pi = 0; pi < (int32_t)darray_size(peg_rules); pi++) {
      if (strcmp(peg_rules[pi].name, scope->name) == 0) {
        parse_scope_name = scope->name;
        break;
      }
    }
  }
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

static DfaPattern* _resolve_body(ScopeInfo* scope, VpaUnit* body, VpaRule* rules, ScopeInfo* scopes,
                                 ActionRegistry* reg, EffectDecl* effects, PegRule* peg_rules) {
  DfaPattern* patterns = darray_new(sizeof(DfaPattern), 0);

  for (int32_t i = 0; i < (int32_t)darray_size(body); i++) {
    VpaUnit* u = &body[i];

    if (u->kind == VPA_REGEXP && u->re) {
      int32_t action_id = _resolve_action(reg, scope, u, NULL, effects, peg_rules, false);
      if (action_id == 0) {
        continue;
      }
      darray_push(patterns,
                  ((DfaPattern){.kind = VPA_REGEXP, .ir = u->re, .state_name = NULL, .action_id = action_id}));

    } else if (u->kind == VPA_REF && u->name) {
      VpaRule* ref_rule = NULL;
      for (int32_t ri = 0; ri < (int32_t)darray_size(rules); ri++) {
        if (strcmp(rules[ri].name, u->name) == 0) {
          ref_rule = &rules[ri];
          break;
        }
      }
      if (!ref_rule) {
        continue;
      }
      VpaUnit* scope_unit = _find_scope_unit(ref_rule);
      if (scope_unit && scope_unit->re) {
        ScopeInfo* target = _find_scope(scopes, ref_rule->name);
        if (target) {
          int32_t aid =
              _register_action(reg, 0, target->scope_id, false, scope_unit->hook, scope_unit->user_hook, NULL);
          darray_push(patterns,
                      ((DfaPattern){.kind = VPA_REGEXP, .ir = scope_unit->re, .state_name = NULL, .action_id = aid}));
        }
      }
      for (int32_t j = 0; j < (int32_t)darray_size(ref_rule->units); j++) {
        VpaUnit* ru = &ref_rule->units[j];
        if (ru->kind == VPA_REGEXP && ru->re) {
          int32_t aid = _resolve_action(reg, scope, ru, ref_rule->name, effects, peg_rules, false);
          if (aid == 0) {
            continue;
          }
          darray_push(patterns, ((DfaPattern){.kind = VPA_REGEXP, .ir = ru->re, .state_name = NULL, .action_id = aid}));
        }
      }

    } else if (u->kind == VPA_SCOPE && u->re) {
      ScopeInfo* target = _find_scope(scopes, u->name ? u->name : "");
      if (target) {
        int32_t aid = _register_action(reg, 0, target->scope_id, false, u->hook, u->user_hook, NULL);
        darray_push(patterns, ((DfaPattern){.kind = VPA_REGEXP, .ir = u->re, .state_name = NULL, .action_id = aid}));
      }
    }
  }

  return patterns;
}

// --- Build DFA from resolved patterns ---

static void _gen_scope_dfa(ScopeInfo* scope, DfaPattern* patterns, IrWriter* w) {
  char func_name[128];
  snprintf(func_name, sizeof(func_name), "lex_%s", scope->name);

  int32_t n_regex = 0;
  for (int32_t i = 0; i < (int32_t)darray_size(patterns); i++) {
    if (patterns[i].kind == VPA_REGEXP && patterns[i].ir) {
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
    if (patterns[i].kind != VPA_REGEXP || !patterns[i].ir) {
      continue;
    }
    if (emitted > 0) {
      re_fork(re);
    }
    DebugInfo di = {0, 0};
    re_ir_exec(re, patterns[i].ir, di);
    re_action(re, patterns[i].action_id);
    emitted++;
  }

  re_rparen(re);
  aut_optimize(aut);
  aut_gen_dfa(aut, w, false);

  re_del(re);
  aut_del(aut);
}

// --- Header generation ---

static void _gen_runtime(HeaderWriter* hw) {
  hw_raw(hw, (const char*)NEST_RT);
  hw_blank(hw);
  hw_raw(hw, "int32_t vpa_rt_read_cp(void* src, int32_t cp_off);\n");
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
  irwriter_declare(w, "void", "tc_add", "ptr, i32, i32, i32, i32");
  irwriter_declare(w, "ptr", "tc_push", "ptr, i32");
  irwriter_declare(w, "ptr", "tc_pop", "ptr");

  bool has_parse = false;
  for (int32_t i = 0; i < (int32_t)darray_size(reg->entries); i++) {
    if (_has_parse_scope(&reg->entries[i])) {
      has_parse = true;
      break;
    }
  }
  if (has_parse) {
    irwriter_declare(w, "i32", "tc_size", "ptr");
    irwriter_declare(w, "void", "tc_parse_begin", "ptr");
    irwriter_declare(w, "void", "tc_parse_end", "");
    irwriter_declare(w, "ptr", "malloc", "i64");
    irwriter_declare(w, "void", "free", "ptr");
    irwriter_raw(w, "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)\n\n");
  }

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

  irwriter_raw(w, "define void @vpa_dispatch(ptr %tt, i32 %action_id, i32 %cp_start, i32 %cp_size, ptr %bt_stack) {\n");
  irwriter_raw(w, "entry:\n");

  if (n == 0) {
    irwriter_raw(w, "  ret void\n");
    irwriter_raw(w, "}\n\n");
    return;
  }

  irwriter_raw(w, "  %idx = sub i32 %action_id, 1\n");
  irwriter_rawf(w, "  %%valid = icmp ult i32 %%idx, %d\n", n);
  irwriter_raw(w, "  br i1 %valid, label %Ljump, label %Ldefault\n");

  irwriter_raw(w, "Ljump:\n");
  irwriter_rawf(w, "  %%labels = alloca [%d x ptr]\n", n);
  irwriter_rawf(w, "  store [%d x ptr] [", n);
  for (int32_t i = 0; i < n; i++) {
    if (i > 0) {
      irwriter_raw(w, ", ");
    }
    irwriter_rawf(w, "ptr blockaddress(@vpa_dispatch, %%Lact_%d)", i + 1);
  }
  irwriter_raw(w, "], ptr %labels\n");
  irwriter_rawf(w, "  %%addr_ptr = getelementptr [%d x ptr], ptr %%labels, i32 0, i32 %%idx\n", n);
  irwriter_raw(w, "  %addr = load ptr, ptr %addr_ptr\n");
  irwriter_raw(w, "  indirectbr ptr %addr, [");
  for (int32_t i = 0; i < n; i++) {
    if (i > 0) {
      irwriter_raw(w, ", ");
    }
    irwriter_rawf(w, "label %%Lact_%d", i + 1);
  }
  irwriter_raw(w, "]\n");

  irwriter_raw(w, "Ldefault:\n  ret void\n");

  for (int32_t i = 0; i < n; i++) {
    ActionEntry* e = &reg->entries[i];
    int32_t aid = i + 1;
    irwriter_rawf(w, "Lact_%d:\n", aid);
    if (_has_user_hook(e)) {
      int32_t sym_len = snprintf(NULL, 0, "vpa_hook_%s", e->user_hook + 1) + 1;
      char symbol[sym_len];
      _make_user_hook_symbol(e->user_hook, symbol, sizeof(symbol));
      irwriter_rawf(w, "  call void @%s(ptr %%tt, i32 %%cp_start, i32 %%cp_size)\n", symbol);
    }
    if (e->tok_id > 0) {
      irwriter_rawf(w, "  call void @tc_add(ptr %%tt, i32 %d, i32 %%cp_start, i32 %%cp_size, i32 -1)\n", e->tok_id);
    }
    if (e->push_scope_id >= 0) {
      irwriter_rawf(w, "  call ptr @tc_push(ptr %%tt, i32 %d)\n", e->push_scope_id);
    }
    if (e->pop_scope) {
      if (_has_parse_scope(e)) {
        irwriter_rawf(w, "  %%ncols0_%d = call i32 @tc_size(ptr %%tt)\n", aid);
        irwriter_rawf(w, "  %%ncols1_%d = add i32 %%ncols0_%d, 1\n", aid, aid);
        irwriter_rawf(w, "  %%ncols64_%d = sext i32 %%ncols1_%d to i64\n", aid, aid);
        irwriter_rawf(w, "  %%elt_end_%d = getelementptr %%Col.%s, ptr null, i32 1\n", aid, e->parse_scope_name);
        irwriter_rawf(w, "  %%elt_size_%d = ptrtoint ptr %%elt_end_%d to i64\n", aid, aid);
        irwriter_rawf(w, "  %%table_size_%d = mul i64 %%ncols64_%d, %%elt_size_%d\n", aid, aid, aid);
        irwriter_rawf(w, "  call void @tc_parse_begin(ptr %%tt)\n");
        irwriter_rawf(w, "  %%table_%d = call ptr @malloc(i64 %%table_size_%d)\n", aid, aid);
        irwriter_rawf(w, "  call void @llvm.memset.p0.i64(ptr %%table_%d, i8 -1, i64 %%table_size_%d, i1 false)\n", aid,
                      aid);
        irwriter_rawf(w, "  %%parse_len_%d = call i32 @parse_%s(ptr %%table_%d, i32 0, ptr %%bt_stack)\n", aid,
                      e->parse_scope_name, aid);
        irwriter_rawf(w, "  call void @free(ptr %%table_%d)\n", aid);
        irwriter_raw(w, "  call void @tc_parse_end()\n");
      }
      irwriter_raw(w, "  call ptr @tc_pop(ptr %tt)\n");
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

static void _gen_lex_loop_ir(ScopeInfo* scopes, IrWriter* w, bool has_parse) {
  irwriter_declare(w, "i32", "vpa_rt_read_cp", "ptr, i32");
  irwriter_declare(w, "i32", "tc_scope", "ptr");

  int32_t n = (int32_t)darray_size(scopes);

  irwriter_raw(w, "define void @vpa_lex(ptr %src, i32 %len, ptr %tt) {\n");
  irwriter_raw(w, "entry:\n");

  irwriter_raw(w, "  %cp_off = alloca i32\n");
  irwriter_raw(w, "  %state = alloca i32\n");
  irwriter_raw(w, "  %tok_start = alloca i32\n");
  irwriter_raw(w, "  %last_act = alloca i32\n");
  irwriter_raw(w, "  %last_off = alloca i32\n");
  irwriter_raw(w, "  %new_state = alloca i32\n");
  irwriter_raw(w, "  %act_id = alloca i32\n");

  irwriter_raw(w, "  store i32 0, ptr %cp_off\n");
  irwriter_raw(w, "  store i32 0, ptr %state\n");
  irwriter_raw(w, "  store i32 0, ptr %tok_start\n");
  irwriter_raw(w, "  store i32 0, ptr %last_act\n");
  irwriter_raw(w, "  store i32 0, ptr %last_off\n");

  if (has_parse) {
    irwriter_raw(w, "  %bt_end = getelementptr %BtStack, ptr null, i32 1\n");
    irwriter_raw(w, "  %bt_size = ptrtoint ptr %bt_end to i64\n");
    irwriter_raw(w, "  %bt_stack = call ptr @malloc(i64 %bt_size)\n");
    irwriter_raw(w, "  %bt_top_ptr = getelementptr %BtStack, ptr %bt_stack, i32 0, i32 1\n");
    irwriter_raw(w, "  store i32 -1, ptr %bt_top_ptr\n");
  }

  irwriter_raw(w, "  br label %Lloop\n\n");

  irwriter_raw(w, "Lloop:\n");
  irwriter_raw(w, "  %off.0 = load i32, ptr %cp_off\n");
  irwriter_raw(w, "  %at_end = icmp sge i32 %off.0, %len\n");
  irwriter_raw(w, "  br i1 %at_end, label %Lflush, label %Lfeed\n\n");

  irwriter_raw(w, "Lfeed:\n");
  irwriter_raw(w, "  %off.1 = load i32, ptr %cp_off\n");
  irwriter_raw(w, "  %cp = call i32 @vpa_rt_read_cp(ptr %src, i32 %off.1)\n");
  irwriter_raw(w, "  %scope = call i32 @tc_scope(ptr %tt)\n");
  irwriter_raw(w, "  %st = load i32, ptr %state\n");
  irwriter_raw(w, "  %st64 = sext i32 %st to i64\n");
  irwriter_raw(w, "  %cp64 = sext i32 %cp to i64\n");

  irwriter_raw(w, "  switch i32 %scope, label %Ldone [\n");
  for (int32_t i = 0; i < n; i++) {
    irwriter_rawf(w, "    i32 %d, label %%Lcall_%d\n", scopes[i].scope_id, scopes[i].scope_id);
  }
  irwriter_raw(w, "  ]\n\n");

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

  irwriter_raw(w, "Lresult:\n");
  irwriter_raw(w, "  %ns = load i32, ptr %new_state\n");
  irwriter_raw(w, "  %ai = load i32, ptr %act_id\n");
  irwriter_raw(w, "  %is_reject = icmp eq i32 %ai, -2\n");
  irwriter_raw(w, "  br i1 %is_reject, label %Lreject, label %Laccept_check\n\n");

  irwriter_raw(w, "Laccept_check:\n");
  irwriter_raw(w, "  %is_action = icmp sgt i32 %ai, 0\n");
  irwriter_raw(w, "  br i1 %is_action, label %Lrecord, label %Ladvance\n\n");

  irwriter_raw(w, "Lrecord:\n");
  irwriter_raw(w, "  store i32 %ai, ptr %last_act\n");
  irwriter_raw(w, "  %off.2 = load i32, ptr %cp_off\n");
  irwriter_raw(w, "  %off.3 = add i32 %off.2, 1\n");
  irwriter_raw(w, "  store i32 %off.3, ptr %last_off\n");
  irwriter_raw(w, "  br label %Ladvance\n\n");

  irwriter_raw(w, "Ladvance:\n");
  irwriter_raw(w, "  store i32 %ns, ptr %state\n");
  irwriter_raw(w, "  %off.4 = load i32, ptr %cp_off\n");
  irwriter_raw(w, "  %off.5 = add i32 %off.4, 1\n");
  irwriter_raw(w, "  store i32 %off.5, ptr %cp_off\n");
  irwriter_raw(w, "  br label %Lloop\n\n");

  irwriter_raw(w, "Lreject:\n");
  irwriter_raw(w, "  %la = load i32, ptr %last_act\n");
  irwriter_raw(w, "  %has_accept = icmp sgt i32 %la, 0\n");
  irwriter_raw(w, "  br i1 %has_accept, label %Ldispatch, label %Lskip\n\n");

  irwriter_raw(w, "Ldispatch:\n");
  irwriter_raw(w, "  %ts = load i32, ptr %tok_start\n");
  irwriter_raw(w, "  %lo = load i32, ptr %last_off\n");
  irwriter_raw(w, "  %sz = sub i32 %lo, %ts\n");
  if (has_parse) {
    irwriter_raw(w, "  call void @vpa_dispatch(ptr %tt, i32 %la, i32 %ts, i32 %sz, ptr %bt_stack)\n");
  } else {
    irwriter_raw(w, "  call void @vpa_dispatch(ptr %tt, i32 %la, i32 %ts, i32 %sz, ptr null)\n");
  }
  irwriter_raw(w, "  store i32 %lo, ptr %cp_off\n");
  irwriter_raw(w, "  store i32 0, ptr %state\n");
  irwriter_raw(w, "  store i32 %lo, ptr %tok_start\n");
  irwriter_raw(w, "  store i32 0, ptr %last_act\n");
  irwriter_raw(w, "  store i32 0, ptr %last_off\n");
  irwriter_raw(w, "  br label %Lloop\n\n");

  irwriter_raw(w, "Lskip:\n");
  irwriter_raw(w, "  %off.6 = load i32, ptr %tok_start\n");
  irwriter_raw(w, "  %off.7 = add i32 %off.6, 1\n");
  irwriter_raw(w, "  store i32 %off.7, ptr %cp_off\n");
  irwriter_raw(w, "  store i32 %off.7, ptr %tok_start\n");
  irwriter_raw(w, "  store i32 0, ptr %state\n");
  irwriter_raw(w, "  store i32 0, ptr %last_act\n");
  irwriter_raw(w, "  store i32 0, ptr %last_off\n");
  irwriter_raw(w, "  br label %Lloop\n\n");

  irwriter_raw(w, "Lflush:\n");
  irwriter_raw(w, "  %la2 = load i32, ptr %last_act\n");
  irwriter_raw(w, "  %has2 = icmp sgt i32 %la2, 0\n");
  irwriter_raw(w, "  br i1 %has2, label %Lflush_dispatch, label %Ldone\n\n");

  irwriter_raw(w, "Lflush_dispatch:\n");
  irwriter_raw(w, "  %ts2 = load i32, ptr %tok_start\n");
  irwriter_raw(w, "  %lo2 = load i32, ptr %last_off\n");
  irwriter_raw(w, "  %sz2 = sub i32 %lo2, %ts2\n");
  if (has_parse) {
    irwriter_raw(w, "  call void @vpa_dispatch(ptr %tt, i32 %la2, i32 %ts2, i32 %sz2, ptr %bt_stack)\n");
  } else {
    irwriter_raw(w, "  call void @vpa_dispatch(ptr %tt, i32 %la2, i32 %ts2, i32 %sz2, ptr null)\n");
  }
  irwriter_raw(w, "  br label %Ldone\n\n");

  irwriter_raw(w, "Ldone:\n");
  if (has_parse) {
    irwriter_raw(w, "  call void @free(ptr %bt_stack)\n");
  }
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
      ScopeInfo s = {
          .name = strdup(rule->name),
          .scope_id = (int32_t)darray_size(scopes),
          .body = rule->units,
          .rule_units = rule->units,
      };
      darray_push(scopes, s);
    } else {
      VpaUnit* su = _find_scope_unit(rule);
      if (su) {
        ScopeInfo s = {
            .name = strdup(rule->name),
            .scope_id = (int32_t)darray_size(scopes),
            .body = su->children,
            .rule_units = rule->units,
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

  _gen_runtime(hw);
  _gen_scope_ids(scopes, hw);

  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    if (!scopes[i].body) {
      continue;
    }
    DfaPattern* patterns = _resolve_body(&scopes[i], scopes[i].body, rules, scopes, &reg, effects, peg_rules);
    _gen_scope_dfa(&scopes[i], patterns, w);
    darray_del(patterns);
  }

  bool has_parse = false;
  for (int32_t i = 0; i < (int32_t)darray_size(reg.entries); i++) {
    if (_has_parse_scope(&reg.entries[i])) {
      has_parse = true;
      break;
    }
  }

  _gen_action_dispatch_ir(&reg, w);
  _gen_lex_loop_ir(scopes, w, has_parse);

  _gen_lex_declarations(scopes, hw);
  _gen_user_hook_header(&reg, hw);
  _gen_token_header(&reg, hw);
  _gen_action_table_header(&reg, scopes, hw);

  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    free(scopes[i].name);
  }
  darray_del(scopes);
  _free_action_registry(&reg);
}
