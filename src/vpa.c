#include "vpa.h"
#include "aut.h"
#include "darray.h"
#include "re.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../build/nest_rt.inc"

// --- Actions table ---
// De-duplicated action_units sequences. action_id = index + 1.

typedef struct {
  int32_t action_id;
  VpaActionUnits action_units; // not owned
} Action;

typedef Action* Actions;

static bool _au_equal(VpaActionUnits a, VpaActionUnits b) {
  int32_t na = (int32_t)darray_size(a);
  int32_t nb = (int32_t)darray_size(b);
  if (na != nb) {
    return false;
  }
  for (int32_t i = 0; i < na; i++) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

static int32_t _find_or_add_action(Actions* acts, VpaActionUnits au) {
  if (!au || (int32_t)darray_size(au) == 0) {
    return 0;
  }
  for (int32_t i = 1; i < (int32_t)darray_size(*acts); i++) {
    if (_au_equal((*acts)[i].action_units, au)) {
      return (*acts)[i].action_id;
    }
  }
  int32_t id = (int32_t)darray_size(*acts);
  Action a = {.action_id = id, .action_units = au};
  darray_push(*acts, a);
  return id;
}

// --- DFA pattern ---

typedef struct {
  ReIr ir;
  int32_t action_id;
} DfaPattern;

// --- Resolve scope body into DFA patterns ---

static DfaPattern* _resolve_body(VpaScope* scope, VpaScope* scopes, Actions* acts) {
  DfaPattern* patterns = darray_new(sizeof(DfaPattern), 0);

  for (int32_t i = 0; i < (int32_t)darray_size(scope->children); i++) {
    VpaUnit* u = &scope->children[i];

    if (u->kind == VPA_RE && u->re) {
      int32_t aid = _find_or_add_action(acts, u->action_units);
      if (aid == 0) {
        continue;
      }
      darray_push(patterns, ((DfaPattern){.ir = u->re, .action_id = aid}));

    } else if (u->kind == VPA_CALL) {
      // Inline the leader regex of the called scope
      VpaScope* target = NULL;
      for (int32_t s = 0; s < (int32_t)darray_size(scopes); s++) {
        if (scopes[s].scope_id == u->call_scope_id) {
          target = &scopes[s];
          break;
        }
      }
      if (target && target->leader.re) {
        int32_t aid = _find_or_add_action(acts, u->action_units);
        if (aid > 0) {
          darray_push(patterns, ((DfaPattern){.ir = target->leader.re, .action_id = aid}));
        }
      }
    }
  }

  return patterns;
}

// --- Build DFA from resolved patterns ---

static void _gen_scope_dfa(VpaScope* scope, DfaPattern* patterns, IrWriter* w) {
  char func_name[128];
  snprintf(func_name, sizeof(func_name), "lex_%s", scope->name);

  int32_t n_regex = 0;
  for (int32_t i = 0; i < (int32_t)darray_size(patterns); i++) {
    if (patterns[i].ir) {
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
    if (!patterns[i].ir) {
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

// --- Header: runtime ---

static void _gen_runtime(HeaderWriter* hw) {
  hw_raw(hw, (const char*)NEST_RT);
  hw_blank(hw);
  hw_raw(hw, "int32_t vpa_rt_read_cp(void* src, int32_t cp_off);\n");
}

// --- Header: token IDs ---

static void _gen_token_header(Symtab* tokens, HeaderWriter* hw) {
  hw_blank(hw);
  hw_comment(hw, "Token IDs");
  int32_t n = symtab_count(tokens);
  for (int32_t i = 0; i < n; i++) {
    int32_t id = tokens->start_num + i;
    const char* name = symtab_get(tokens, id);
    int32_t dn_len = snprintf(NULL, 0, "TOK_%s", name) + 1;
    char define_name[dn_len];
    snprintf(define_name, (size_t)dn_len, "TOK_%s", name);
    for (char* p = define_name + 4; *p; p++) {
      if (*p >= 'a' && *p <= 'z') {
        *p -= 32;
      } else if (*p == '.') {
        *p = '_';
      }
    }
    hw_define(hw, define_name, id);
  }
}

// --- Header: hook IDs ---

static void _gen_hook_header(Symtab* hooks, HeaderWriter* hw) {
  hw_blank(hw);
  hw_comment(hw, "Hook IDs (action_unit_id = -hook_id)");
  int32_t n = symtab_count(hooks);
  for (int32_t i = 0; i < n; i++) {
    int32_t id = hooks->start_num + i;
    const char* name = symtab_get(hooks, id);
    // Skip the leading '.' if present
    const char* sym = name[0] == '.' ? name + 1 : name;
    int32_t dn_len = snprintf(NULL, 0, "HOOK_%s", sym) + 1;
    char define_name[dn_len];
    snprintf(define_name, (size_t)dn_len, "HOOK_%s", sym);
    for (char* p = define_name + 5; *p; p++) {
      if (*p >= 'a' && *p <= 'z') {
        *p -= 32;
      } else if (*p == '.') {
        *p = '_';
      }
    }
    hw_define(hw, define_name, -id);
  }
}

// --- Header: scope IDs ---

static void _gen_scope_ids(VpaScope* scopes, HeaderWriter* hw) {
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

// --- Header: ParseContext, ParseResult, etc. ---

static void _gen_user_hook_header(Symtab* hooks, HeaderWriter* hw) {
  // User hooks start after builtins (index >= HOOK_ID_BUILTIN_COUNT)
  int32_t n = symtab_count(hooks);
  if (n <= HOOK_ID_BUILTIN_COUNT) {
    return;
  }

  hw_blank(hw);
  hw_raw(hw, "typedef int32_t (*LexHook)(void* userdata, Token* token, const char* token_str_start);\n\n");

  hw_raw(hw, "typedef struct {\n");
  hw_raw(hw, "  void* userdata;\n");
  for (int32_t i = HOOK_ID_BUILTIN_COUNT; i < n; i++) {
    int32_t id = hooks->start_num + i;
    const char* name = symtab_get(hooks, id);
    const char* sym = name[0] == '.' ? name + 1 : name;
    hw_fmt(hw, "  LexHook %s;\n", sym);
  }
  hw_raw(hw, "} ParseContext;\n");
}

static void _gen_parse_error_header(HeaderWriter* hw) {
  hw_blank(hw);
  hw_raw(hw, "typedef enum {\n");
  hw_raw(hw, "  PARSE_ERROR_INVALID_HOOK,\n");
  hw_raw(hw, "  PARSE_ERROR_REQUIRE_MORE_INPUT,\n");
  hw_raw(hw, "  PARSE_ERROR_TOKEN_ERR,\n");
  hw_raw(hw, "  PARSE_ERROR_INVALID_SYNTAX,\n");
  hw_raw(hw, "} ParseErrorType;\n\n");

  hw_raw(hw, "typedef struct {\n");
  hw_raw(hw, "  const char* message;\n");
  hw_raw(hw, "  ParseErrorType type;\n");
  hw_raw(hw, "  int32_t cp_offset;\n");
  hw_raw(hw, "  int32_t cp_size;\n");
  hw_raw(hw, "} ParseError;\n");
  hw_raw(hw, "typedef ParseError* ParseErrors;\n");
}

static void _gen_parse_result_header(HeaderWriter* hw) {
  hw_blank(hw);
  hw_raw(hw, "typedef struct {\n");
  hw_raw(hw, "  void* main;\n");
  hw_raw(hw, "  TokenTree* tt;\n");
  hw_raw(hw, "  ParseErrors errors;\n");
  hw_raw(hw, "} ParseResult;\n");
}

// --- Header: lex declarations ---

static void _gen_lex_declarations(VpaScope* scopes, HeaderWriter* hw) {
  hw_blank(hw);
  hw_comment(hw, "Lexer declarations");
  hw_raw(hw, "typedef struct { int64_t state; int64_t action; } LexResult;\n");
  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    hw_fmt(hw, "extern LexResult lex_%s(int64_t state, int64_t cp);\n", scopes[i].name);
  }
  hw_raw(hw, "extern void vpa_lex(int64_t src, int64_t len, int64_t tt);\n");
}

// --- Header: action / scope table sizes ---

static void _gen_action_table_header(Actions* acts, VpaScope* scopes, HeaderWriter* hw) {
  hw_blank(hw);
  hw_fmt(hw, "#define VPA_N_ACTIONS %d\n", (int32_t)darray_size(*acts) - 1); // exclude entry 0
  hw_fmt(hw, "#define VPA_N_SCOPES %d\n", (int32_t)darray_size(scopes));
}

// --- Header: main parse function ---

static void _gen_main_func_header(const char* prefix, Symtab* hooks, HeaderWriter* hw) {
  hw_blank(hw);
  hw_comment(hw, "Main parse and cleanup functions");
  bool has_user_hooks = symtab_count(hooks) > HOOK_ID_BUILTIN_COUNT;
  if (has_user_hooks) {
    hw_fmt(hw, "extern ParseResult %s_parse(ParseContext lc, UStr src);\n", prefix);
  } else {
    hw_fmt(hw, "extern ParseResult %s_parse(UStr src);\n", prefix);
  }
  hw_fmt(hw, "extern void %s_cleanup(ParseResult r);\n", prefix);
}

// --- IR: action dispatch ---

static bool _effect_has(EffectDecls effects, int32_t hook_id, int32_t target_au) {
  for (int32_t i = 0; i < (int32_t)darray_size(effects); i++) {
    if (effects[i].hook_id == hook_id) {
      for (int32_t j = 0; j < (int32_t)darray_size(effects[i].effects); j++) {
        if (effects[i].effects[j] == target_au) {
          return true;
        }
      }
    }
  }
  return false;
}

// Check if any action has a .end hook (directly or via effects)
static bool _action_pops(VpaActionUnits au, EffectDecls effects) {
  for (int32_t i = 0; i < (int32_t)darray_size(au); i++) {
    if (au[i] <= 0) {
      int32_t hid = -au[i];
      if (hid == HOOK_ID_END) {
        return true;
      }
      if (_effect_has(effects, hid, -HOOK_ID_END)) {
        return true;
      }
    }
  }
  return false;
}

static void _gen_action_dispatch_ir(Actions* acts, VpaScope* scopes, Symtab* hooks, EffectDecls effects, IrWriter* w) {
  irwriter_declare(w, "void", "tc_add", "ptr, i32, i32, i32, i32");
  irwriter_declare(w, "ptr", "tc_push", "ptr, i32");
  irwriter_declare(w, "ptr", "tc_pop", "ptr");

  bool has_parse = false;
  for (int32_t i = 1; i < (int32_t)darray_size(*acts); i++) {
    VpaActionUnits au = (*acts)[i].action_units;
    for (int32_t s = 0; s < (int32_t)darray_size(scopes); s++) {
      if (scopes[s].has_parser && _action_pops(au, effects)) {
        has_parse = true;
        break;
      }
    }
    if (has_parse) {
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

  // Declare user hook callbacks
  int32_t n_hooks = symtab_count(hooks);
  for (int32_t i = HOOK_ID_BUILTIN_COUNT; i < n_hooks; i++) {
    int32_t id = hooks->start_num + i;
    const char* name = symtab_get(hooks, id);
    const char* sym = name[0] == '.' ? name + 1 : name;
    char symbol[128];
    snprintf(symbol, sizeof(symbol), "vpa_hook_%s", sym);
    irwriter_declare(w, "void", symbol, "ptr, i32, i32");
  }

  int32_t n = (int32_t)darray_size(*acts) - 1; // actions indexed from 1

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
    int32_t aid = i + 1;
    VpaActionUnits au = (*acts)[aid].action_units;
    irwriter_rawf(w, "Lact_%d:\n", aid);

    for (int32_t j = 0; j < (int32_t)darray_size(au); j++) {
      int32_t auid = au[j];
      if (auid > 0) {
        // Emit token
        irwriter_rawf(w, "  call void @tc_add(ptr %%tt, i32 %d, i32 %%cp_start, i32 %%cp_size, i32 -1)\n", auid);
      } else {
        int32_t hook_id = -auid;
        if (hook_id == HOOK_ID_BEGIN) {
          // Find which scope to push — look at call_scope_id from the unit
          // For now, use a default. The push scope_id is encoded in the action.
          // Actually, we need to find the scope_id to push. This is determined by
          // the VPA_CALL unit that generated this action. We need to pass it through.
          // For now, push scope 0 as placeholder — this will be fixed with proper scope tracking.
          irwriter_rawf(w, "  call ptr @tc_push(ptr %%tt, i32 0)\n");
        } else if (hook_id == HOOK_ID_END) {
          // Check if the scope we're popping has a parser
          // Since we don't know the scope at compile time in this dispatch,
          // we match the old approach: check all scopes that have parsers.
          // The old code stored parse_scope_name per action entry.
          irwriter_raw(w, "  call ptr @tc_pop(ptr %tt)\n");
        } else if (hook_id == HOOK_ID_FAIL) {
          irwriter_raw(w, "  ret void\n");
          goto next_action;
        } else if (hook_id == HOOK_ID_UNPARSE) {
          // unparse: no-op in dispatch, handled by lex loop
        } else {
          // User hook
          const char* name = symtab_get(hooks, hook_id);
          const char* sym = name[0] == '.' ? name + 1 : name;
          char symbol[128];
          snprintf(symbol, sizeof(symbol), "vpa_hook_%s", sym);
          irwriter_rawf(w, "  call void @%s(ptr %%tt, i32 %%cp_start, i32 %%cp_size)\n", symbol);
        }
      }
    }
    irwriter_raw(w, "  ret void\n");
  next_action:;
  }

  irwriter_raw(w, "}\n\n");
}

// --- IR: outer lexing loop ---

static void _gen_lex_loop_ir(VpaScope* scopes, IrWriter* w, bool has_parse) {
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

// --- IR: main parse function ---

static void _gen_main_func_ir(const char* prefix, IrWriter* w) {
  irwriter_rawf(w, "define void @%s_parse(ptr %%src, i32 %%len, ptr %%tt) {\n", prefix);
  irwriter_raw(w, "entry:\n");
  irwriter_raw(w, "  call void @vpa_lex(ptr %src, i32 %len, ptr %tt)\n");
  irwriter_raw(w, "  %ncols0 = call i32 @tc_size(ptr %tt)\n");
  irwriter_raw(w, "  %ncols1 = add i32 %ncols0, 1\n");
  irwriter_raw(w, "  %ncols64 = sext i32 %ncols1 to i64\n");
  irwriter_raw(w, "  %elt_end = getelementptr %Col.main, ptr null, i32 1\n");
  irwriter_raw(w, "  %elt_size = ptrtoint ptr %elt_end to i64\n");
  irwriter_raw(w, "  %table_size = mul i64 %ncols64, %elt_size\n");
  irwriter_raw(w, "  %bt_end = getelementptr %BtStack, ptr null, i32 1\n");
  irwriter_raw(w, "  %bt_size = ptrtoint ptr %bt_end to i64\n");
  irwriter_raw(w, "  %bt_stack = call ptr @malloc(i64 %bt_size)\n");
  irwriter_raw(w, "  %bt_top_ptr = getelementptr %BtStack, ptr %bt_stack, i32 0, i32 1\n");
  irwriter_raw(w, "  store i32 -1, ptr %bt_top_ptr\n");
  irwriter_raw(w, "  %table = call ptr @malloc(i64 %table_size)\n");
  irwriter_raw(w, "  call void @llvm.memset.p0.i64(ptr %table, i8 -1, i64 %table_size, i1 false)\n");
  irwriter_raw(w, "  %parse_len = call i32 @parse_main(ptr %table, i32 0, ptr %bt_stack)\n");
  irwriter_raw(w, "  call void @free(ptr %table)\n");
  irwriter_raw(w, "  call void @free(ptr %bt_stack)\n");
  irwriter_raw(w, "  ret void\n");
  irwriter_raw(w, "}\n\n");
}

// --- Public API ---

void vpa_gen(VpaGenInput* input, HeaderWriter* hw, IrWriter* w, const char* prefix) {
  VpaScope* scopes = input->scopes;
  EffectDecls effects = input->effect_decls;
  Symtab* tokens = &input->tokens;
  Symtab* hooks = &input->hooks;

  // Build actions table. Entry 0 is empty sentinel.
  Actions acts = darray_new(sizeof(Action), 0);
  darray_push(acts, ((Action){0}));

  _gen_runtime(hw);
  _gen_scope_ids(scopes, hw);

  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    DfaPattern* patterns = _resolve_body(&scopes[i], scopes, &acts);
    _gen_scope_dfa(&scopes[i], patterns, w);
    darray_del(patterns);
  }

  bool has_parse = false;
  for (int32_t i = 1; i < (int32_t)darray_size(acts); i++) {
    if (_action_pops(acts[i].action_units, effects)) {
      for (int32_t s = 0; s < (int32_t)darray_size(scopes); s++) {
        if (scopes[s].has_parser) {
          has_parse = true;
          break;
        }
      }
      if (has_parse) {
        break;
      }
    }
  }

  _gen_action_dispatch_ir(&acts, scopes, hooks, effects, w);
  _gen_lex_loop_ir(scopes, w, has_parse);

  if (prefix) {
    _gen_main_func_ir(prefix, w);
  }

  _gen_lex_declarations(scopes, hw);
  _gen_user_hook_header(hooks, hw);
  _gen_token_header(tokens, hw);
  _gen_hook_header(hooks, hw);
  _gen_parse_error_header(hw);
  _gen_parse_result_header(hw);
  _gen_action_table_header(&acts, scopes, hw);

  if (prefix) {
    _gen_main_func_header(prefix, hooks, hw);
  }

  darray_del(acts);
}
