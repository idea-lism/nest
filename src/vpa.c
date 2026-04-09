#include "vpa.h"
#include "darray.h"
#include "re.h"
#include "re_ir.h"
#include "symtab.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../build/nest_rt.inc"

// --- Actions table (deduplication) ---

typedef struct {
  int32_t action_id;
  VpaActionUnits action_units; // not owned
} Action;

typedef Action* Actions;

static bool _au_equal(VpaActionUnits a, VpaActionUnits b) {
  if (!a && !b) {
    return true;
  }
  if (!a || !b) {
    return false;
  }
  int32_t na = (int32_t)darray_size(a);
  int32_t nb = (int32_t)darray_size(b);
  if (na != nb) {
    return false;
  }
  return memcmp(a, b, (size_t)na * sizeof(int32_t)) == 0;
}

static int32_t _intern_action(Actions* actions, VpaActionUnits au) {
  if (!au || darray_size(au) == 0) {
    return 0;
  }
  int32_t n = (int32_t)darray_size(*actions);
  for (int32_t i = 1; i < n; i++) {
    if (_au_equal((*actions)[i].action_units, au)) {
      return i;
    }
  }
  Action a = {.action_id = n, .action_units = au};
  darray_push(*actions, a);
  return n;
}

static EffectDecl* _find_effect(VpaGenInput* input, int32_t hook_id) {
  int32_t n = (int32_t)darray_size(input->effect_decls);
  for (int32_t i = 0; i < n; i++) {
    if (input->effect_decls[i].hook_id == hook_id) {
      return &input->effect_decls[i];
    }
  }
  return NULL;
}

// --- DFA generation per scope ---

static void _gen_scope_dfa(VpaGenInput* input, IrWriter* w, VpaScope* scope, Actions* actions) {
  char func_name[128];
  snprintf(func_name, sizeof(func_name), "_dfa_%s", scope->name);

  Aut* aut = aut_new(func_name, "vpa");
  Re* re = re_new(aut);
  DebugInfo di = {0, 0};

  int32_t n_children = scope->children ? (int32_t)darray_size(scope->children) : 0;
  bool started = false;

  re_lparen(re);

  for (int32_t i = 0; i < n_children; i++) {
    VpaUnit* u = &scope->children[i];
    int32_t aid = _intern_action(actions, u->action_units);

    ReIr ir = NULL;
    if (u->kind == VPA_RE) {
      ir = u->re;
    } else if (u->kind == VPA_CALL) {
      for (int32_t s = 0; s < (int32_t)darray_size(input->scopes); s++) {
        if (input->scopes[s].scope_id == u->call_scope_id) {
          ir = input->scopes[s].leader.re;
          break;
        }
      }
    }

    if (!ir || darray_size(ir) == 0) {
      continue;
    }

    if (started) {
      re_fork(re);
    }
    started = true;

    re_ir_exec(re, ir, di);
    re_action(re, aid);
  }

  re_rparen(re);
  aut_optimize(aut);
  aut_gen_dfa(aut, w, false);

  // widened wrapper: lex_{name}(i32, i32) -> {i64, i64}
  char wrapper_name[128];
  snprintf(wrapper_name, sizeof(wrapper_name), "lex_%s", scope->name);

  const char* arg_types[] = {"i32", "i32"};
  const char* arg_names[] = {"state", "cp"};
  irwriter_define_start(w, wrapper_name, "{i64, i64}", 2, arg_types, arg_names);
  irwriter_bb(w);

  IrVal result = irwriter_call_retf(w, "{i32, i32}", func_name, "i32 %%state, i32 %%cp");
  IrVal new_state = irwriter_extractvalue(w, "{i32, i32}", result, 0);
  IrVal action_id = irwriter_extractvalue(w, "{i32, i32}", result, 1);
  IrVal ns64 = irwriter_sext(w, "i32", new_state, "i64");
  IrVal ai64 = irwriter_sext(w, "i32", action_id, "i64");
  IrVal r0 = irwriter_insertvalue(w, "{i64, i64}", -1, "i64", ns64, 0);
  IrVal r1 = irwriter_insertvalue(w, "{i64, i64}", r0, "i64", ai64, 1);
  irwriter_ret(w, "{i64, i64}", r1);
  irwriter_define_end(w);

  re_del(re);
  aut_del(aut);
}

// --- Dispatch function ---

static void _gen_dispatch(VpaGenInput* input, IrWriter* w, Actions actions) {
  int32_t n_actions = (int32_t)darray_size(actions);

  irwriter_declare(w, "void", "tt_add", "i8*, i32, i32, i32, i32");
  irwriter_declare(w, "i8*", "tt_push", "i8*, i32");
  irwriter_declare(w, "i8*", "tt_pop", "i8*");
  irwriter_declare(w, "void", "vpa_error_add", "i8*, i32, i32, i32");

  // pre-declare user hooks
  for (int32_t i = 1; i < n_actions; i++) {
    VpaActionUnits au = actions[i].action_units;
    int32_t n_au = au ? (int32_t)darray_size(au) : 0;
    for (int32_t j = 0; j < n_au; j++) {
      int32_t auid = au[j];
      if (auid <= 0) {
        int32_t hook_id = -auid;
        if (hook_id >= HOOK_ID_BUILTIN_COUNT) {
          const char* hook_name = symtab_get(&input->hooks, hook_id);
          char fn_name[128];
          snprintf(fn_name, sizeof(fn_name), "vpa_hook_%s", hook_name + 1);
          irwriter_declare(w, "i32", fn_name, "i8*, i8*, i8*");
        }
      }
    }
  }

  const char* arg_types[] = {"i32", "i8*", "i32", "i32", "i8*", "i8*"};
  const char* arg_names[] = {"action_id", "tt", "cp_start", "cp_size", "ctx", "errors"};
  irwriter_define_start(w, "vpa_dispatch", "void", 6, arg_types, arg_names);
  irwriter_bb(w);

  if (n_actions <= 1) {
    irwriter_ret_void(w);
    irwriter_define_end(w);
    return;
  }

  int32_t default_bb = irwriter_label(w);
  int32_t* case_bbs = malloc((size_t)n_actions * sizeof(int32_t));
  for (int32_t i = 1; i < n_actions; i++) {
    case_bbs[i] = irwriter_label(w);
  }

  irwriter_switch_start(w, "i32", irwriter_imm(w, "%action_id"), default_bb);
  for (int32_t i = 1; i < n_actions; i++) {
    irwriter_switch_case(w, "i32", i, case_bbs[i]);
  }
  irwriter_switch_end(w);

  for (int32_t i = 1; i < n_actions; i++) {
    irwriter_bb_at(w, case_bbs[i]);

    VpaActionUnits au = actions[i].action_units;
    int32_t n_au = au ? (int32_t)darray_size(au) : 0;
    for (int32_t j = 0; j < n_au; j++) {
      int32_t auid = au[j];
      if (auid > 0) {
        // emit token
        irwriter_call_void_fmtf(w, "tt_add", "i8* %%tt, i32 %d, i32 %%cp_start, i32 %%cp_size, i32 -1", auid);
      } else {
        int32_t hook_id = -auid;
        if (hook_id == HOOK_ID_BEGIN) {
          // push scope onto VPA stack
          irwriter_call_retf(w, "i8*", "tt_push", "i8* %%tt, i32 0");
        } else if (hook_id == HOOK_ID_END) {
          // pop scope from VPA stack
          irwriter_call_retf(w, "i8*", "tt_pop", "i8* %%tt");
        } else if (hook_id >= HOOK_ID_BUILTIN_COUNT) {
          // user hook
          const char* hook_name = symtab_get(&input->hooks, hook_id);
          char fn_name[128];
          snprintf(fn_name, sizeof(fn_name), "vpa_hook_%s", hook_name + 1);
          IrVal ret_val = irwriter_call_retf(w, "i32", fn_name, "i8* %%ctx, i8* null, i8* null");

          // if this hook has %effect, validate the return value
          EffectDecl* ed = _find_effect(input, hook_id);
          if (ed) {
            int32_t n_effects = (int32_t)darray_size(ed->effects);
            // build a chain: check ret_val against each valid effect, if none match -> error
            int32_t ok_bb = irwriter_label(w);
            int32_t err_bb = irwriter_label(w);
            int32_t* effect_bbs = NULL;
            if (n_effects > 0) {
              effect_bbs = malloc((size_t)n_effects * sizeof(int32_t));
              for (int32_t e = 0; e < n_effects; e++) {
                effect_bbs[e] = irwriter_label(w);
              }
            }

            // switch on the return value
            irwriter_switch_start(w, "i32", ret_val, err_bb);
            for (int32_t e = 0; e < n_effects; e++) {
              irwriter_switch_case(w, "i32", ed->effects[e], effect_bbs[e]);
            }
            irwriter_switch_end(w);

            // each valid effect: execute it, then jump to ok
            for (int32_t e = 0; e < n_effects; e++) {
              irwriter_bb_at(w, effect_bbs[e]);
              int32_t ev = ed->effects[e];
              if (ev > 0) {
                irwriter_call_void_fmtf(w, "tt_add", "i8* %%tt, i32 %d, i32 %%cp_start, i32 %%cp_size, i32 -1", ev);
              }
              // builtin hook effects (e.g. .fail) would be handled here
              irwriter_br(w, ok_bb);
            }

            // error: invalid return from hook
            irwriter_bb_at(w, err_bb);
            irwriter_call_void_fmtf(w, "vpa_error_add", "i8* %%errors, i32 0, i32 %%cp_start, i32 %%cp_size");
            irwriter_br(w, ok_bb);

            irwriter_bb_at(w, ok_bb);
            free(effect_bbs);
          }
        }
      }
    }
    irwriter_ret_void(w);
  }

  irwriter_bb_at(w, default_bb);
  irwriter_ret_void(w);

  irwriter_define_end(w);
  free(case_bbs);
}

// --- Main lex loop ---

static void _gen_vpa_lex(VpaGenInput* input, IrWriter* w) {
  int32_t n_scopes = (int32_t)darray_size(input->scopes);

  irwriter_declare(w, "i32", "vpa_rt_read_cp", "i8*, i32");
  irwriter_declare(w, "void", "vpa_error_add", "i8*, i32, i32, i32");
  irwriter_declare(w, "i32", "tt_depth", "i8*");
  irwriter_declare(w, "void", "tt_add", "i8*, i32, i32, i32, i32");

  const char* arg_types[] = {"i8*", "i32", "i8*", "i8*", "i8*"};
  const char* arg_names[] = {"src", "len", "tt", "errors", "ctx"};
  irwriter_define_start(w, "vpa_lex", "void", 5, arg_types, arg_names);

  irwriter_bb(w);

  IrVal cp_off_ptr = irwriter_alloca(w, "i32");
  IrVal dfa_state_ptr = irwriter_alloca(w, "i32");
  IrVal last_action_ptr = irwriter_alloca(w, "i32");
  IrVal last_match_off_ptr = irwriter_alloca(w, "i32");
  IrVal token_start_ptr = irwriter_alloca(w, "i32");

  irwriter_store(w, "i32", irwriter_imm_int(w, 0), cp_off_ptr);
  irwriter_store(w, "i32", irwriter_imm_int(w, 0), dfa_state_ptr);
  irwriter_store(w, "i32", irwriter_imm_int(w, 0), last_action_ptr);
  irwriter_store(w, "i32", irwriter_imm_int(w, 0), last_match_off_ptr);
  irwriter_store(w, "i32", irwriter_imm_int(w, 0), token_start_ptr);

  int32_t loop_bb = irwriter_label(w);
  int32_t read_bb = irwriter_label(w);
  int32_t dispatch_bb = irwriter_label(w);
  int32_t dispatch_action_bb = irwriter_label(w);
  int32_t done_bb = irwriter_label(w);

  irwriter_br(w, loop_bb);

  // --- loop: check if at end ---
  irwriter_bb_at(w, loop_bb);
  IrVal cp_off = irwriter_load(w, "i32", cp_off_ptr);
  IrVal at_end = irwriter_icmp(w, "sge", "i32", cp_off, irwriter_imm(w, "%len"));
  irwriter_br_cond(w, at_end, dispatch_bb, read_bb);

  // --- read cp and run DFA ---
  irwriter_bb_at(w, read_bb);
  IrVal cp_off2 = irwriter_load(w, "i32", cp_off_ptr);
  IrVal cp = irwriter_call_retf(w, "i32", "vpa_rt_read_cp", "i8* %%src, i32 %%r%d", (int)cp_off2);
  IrVal dfa_state = irwriter_load(w, "i32", dfa_state_ptr);

  if (n_scopes > 0) {
    char dfa_fn[128];
    snprintf(dfa_fn, sizeof(dfa_fn), "_dfa_%s", input->scopes[0].name);
    IrVal result = irwriter_call_retf(w, "{i32, i32}", dfa_fn, "i32 %%r%d, i32 %%r%d", (int)dfa_state, (int)cp);
    IrVal new_state = irwriter_extractvalue(w, "{i32, i32}", result, 0);
    IrVal action = irwriter_extractvalue(w, "{i32, i32}", result, 1);

    int32_t has_action_bb = irwriter_label(w);
    int32_t no_action_bb = irwriter_label(w);
    IrVal action_valid = irwriter_icmp(w, "ne", "i32", action, irwriter_imm_int(w, -2));
    irwriter_br_cond(w, action_valid, has_action_bb, no_action_bb);

    irwriter_bb_at(w, has_action_bb);
    int32_t action_pos_bb = irwriter_label(w);
    int32_t advance_bb = irwriter_label(w);
    IrVal action_is_pos = irwriter_icmp(w, "sgt", "i32", action, irwriter_imm_int(w, 0));
    irwriter_br_cond(w, action_is_pos, action_pos_bb, advance_bb);

    irwriter_bb_at(w, action_pos_bb);
    irwriter_store(w, "i32", action, last_action_ptr);
    IrVal next_off = irwriter_binop(w, "add", "i32", cp_off2, irwriter_imm_int(w, 1));
    irwriter_store(w, "i32", next_off, last_match_off_ptr);
    irwriter_br(w, advance_bb);

    irwriter_bb_at(w, advance_bb);
    IrVal adv_off = irwriter_binop(w, "add", "i32", cp_off2, irwriter_imm_int(w, 1));
    irwriter_store(w, "i32", adv_off, cp_off_ptr);
    irwriter_store(w, "i32", new_state, dfa_state_ptr);
    irwriter_br(w, loop_bb);

    irwriter_bb_at(w, no_action_bb);
    irwriter_br(w, dispatch_bb);
  } else {
    irwriter_br(w, done_bb);
  }

  // --- dispatch ---
  irwriter_bb_at(w, dispatch_bb);
  IrVal last_action = irwriter_load(w, "i32", last_action_ptr);
  IrVal tok_start = irwriter_load(w, "i32", token_start_ptr);
  IrVal match_off = irwriter_load(w, "i32", last_match_off_ptr);
  IrVal tok_size = irwriter_binop(w, "sub", "i32", match_off, tok_start);

  IrVal has_last = irwriter_icmp(w, "sgt", "i32", last_action, irwriter_imm_int(w, 0));
  irwriter_br_cond(w, has_last, dispatch_action_bb, done_bb);

  irwriter_bb_at(w, dispatch_action_bb);
  irwriter_call_void_fmtf(w, "vpa_dispatch", "i32 %%r%d, i8* %%tt, i32 %%r%d, i32 %%r%d, i8* %%ctx, i8* %%errors",
                          (int)last_action, (int)tok_start, (int)tok_size);

  irwriter_store(w, "i32", match_off, cp_off_ptr);
  irwriter_store(w, "i32", match_off, token_start_ptr);
  irwriter_store(w, "i32", irwriter_imm_int(w, 0), dfa_state_ptr);
  irwriter_store(w, "i32", irwriter_imm_int(w, 0), last_action_ptr);
  irwriter_store(w, "i32", match_off, last_match_off_ptr);

  IrVal still_going = irwriter_icmp(w, "slt", "i32", match_off, irwriter_imm(w, "%len"));
  irwriter_br_cond(w, still_going, loop_bb, done_bb);

  // --- done ---
  irwriter_bb_at(w, done_bb);
  irwriter_ret_void(w);

  irwriter_define_end(w);
}

// --- {prefix}_parse / {prefix}_cleanup ---

// ParseResult = {PegRef, TokenTree*, ParseErrors} = {i8*, i32, i32, i8*, i8*}
#define PARSE_RESULT_TY "{i8*, i32, i32, i8*, i8*}"

static void _gen_parse_entry(IrWriter* w, const char* prefix) {
  if (!prefix) {
    return;
  }

  irwriter_declare(w, "void", "vpa_lex", "i8*, i32, i8*, i8*, i8*");
  irwriter_declare(w, "i8*", "tt_tree_new", "i8*");
  irwriter_declare(w, "i32", "ustr_size", "i8*");

  // {prefix}_parse(ctx_ptr, src) -> ParseResult
  char parse_name[128];
  snprintf(parse_name, sizeof(parse_name), "%s_parse", prefix);
  const char* parse_arg_types[] = {"i8*", "i8*"};
  const char* parse_arg_names[] = {"ctx", "src"};
  irwriter_define_start(w, parse_name, PARSE_RESULT_TY, 2, parse_arg_types, parse_arg_names);
  irwriter_bb(w);
  IrVal len = irwriter_call_retf(w, "i32", "ustr_size", "i8* %%src");
  IrVal tt = irwriter_call_retf(w, "i8*", "tt_tree_new", "i8* %%src");
  irwriter_call_void_fmtf(w, "vpa_lex", "i8* %%src, i32 %%r%d, i8* %%r%d, i8* null, i8* %%ctx", (int)len, (int)tt);

  IrVal r0 = irwriter_insertvalue(w, PARSE_RESULT_TY, -1, "i8*", irwriter_imm(w, "null"), 0);
  IrVal r1 = irwriter_insertvalue(w, PARSE_RESULT_TY, r0, "i32", irwriter_imm_int(w, 0), 1);
  IrVal r2 = irwriter_insertvalue(w, PARSE_RESULT_TY, r1, "i32", irwriter_imm_int(w, 0), 2);
  IrVal r3 = irwriter_insertvalue(w, PARSE_RESULT_TY, r2, "i8*", tt, 3);
  IrVal r4 = irwriter_insertvalue(w, PARSE_RESULT_TY, r3, "i8*", irwriter_imm(w, "null"), 4);
  irwriter_ret(w, PARSE_RESULT_TY, r4);
  irwriter_define_end(w);

  // {prefix}_cleanup(result) -> void
  char cleanup_name[128];
  snprintf(cleanup_name, sizeof(cleanup_name), "%s_cleanup", prefix);
  const char* cleanup_arg_types[] = {PARSE_RESULT_TY};
  const char* cleanup_arg_names[] = {"res"};
  irwriter_define_start(w, cleanup_name, "void", 1, cleanup_arg_types, cleanup_arg_names);
  irwriter_bb(w);
  irwriter_ret_void(w);
  irwriter_define_end(w);
}

// --- Header generation ---

static void _hw_upper(HeaderWriter* hw, const char* s) {
  for (; *s; s++) {
    hw_rawc(hw, (char)toupper((unsigned char)*s));
  }
}

static void _gen_header(VpaGenInput* input, HeaderWriter* hw, int32_t n_actions, const char* prefix) {
  hw_pragma_once(hw);
  hw_blank(hw);

  // inline amalgamated runtime
  hw_raw(hw, (const char*)NEST_RT);
  hw_blank(hw);

  int32_t n_scopes = (int32_t)darray_size(input->scopes);

  // scope defines
  for (int32_t i = 0; i < n_scopes; i++) {
    hw_raw(hw, "#define SCOPE_");
    _hw_upper(hw, input->scopes[i].name);
    hw_fmt(hw, " %d\n", input->scopes[i].scope_id);
  }
  hw_fmt(hw, "#define SCOPE_COUNT %d\n", n_scopes);
  hw_blank(hw);

  // token defines — skip keyword literals ("lit." prefix)
  int32_t n_tokens = symtab_count(&input->tokens);
  for (int32_t i = 0; i < n_tokens; i++) {
    int32_t tok_id = i + input->tokens.start_num;
    const char* name = symtab_get(&input->tokens, tok_id);
    if (strncmp(name, "lit.", 4) == 0) {
      continue;
    }
    hw_raw(hw, "#define TOK_");
    _hw_upper(hw, name);
    hw_fmt(hw, " %d\n", tok_id);
  }
  hw_blank(hw);

  // builtin hook ID defines
  hw_fmt(hw, "#define HOOK_BEGIN %d\n", -HOOK_ID_BEGIN);
  hw_fmt(hw, "#define HOOK_END %d\n", -HOOK_ID_END);
  hw_fmt(hw, "#define HOOK_FAIL %d\n", -HOOK_ID_FAIL);
  hw_fmt(hw, "#define HOOK_UNPARSE %d\n", -HOOK_ID_UNPARSE);
  hw_blank(hw);

  // user hook defines
  int32_t n_hooks = symtab_count(&input->hooks);
  bool has_user_hooks = false;
  for (int32_t i = HOOK_ID_BUILTIN_COUNT; i < n_hooks + input->hooks.start_num; i++) {
    has_user_hooks = true;
    const char* name = symtab_get(&input->hooks, i);
    hw_raw(hw, "#define HOOK_");
    _hw_upper(hw, name + 1);
    hw_fmt(hw, " %d\n", -i);
  }
  if (has_user_hooks) {
    hw_blank(hw);
  }

  // N_ACTIONS
  hw_fmt(hw, "#define VPA_N_ACTIONS %d\n", n_actions - 1);
  hw_blank(hw);

  // ParseErrorType enum
  hw_raw(hw, "typedef enum {\n");
  hw_raw(hw, "  PARSE_ERROR_INVALID_HOOK,\n");
  hw_raw(hw, "  PARSE_ERROR_REQUIRE_MORE_INPUT,\n");
  hw_raw(hw, "  PARSE_ERROR_TOKEN_ERR,\n");
  hw_raw(hw, "  PARSE_ERROR_INVALID_SYNTAX,\n");
  hw_raw(hw, "} ParseErrorType;\n");
  hw_blank(hw);

  // ParseError
  hw_raw(hw, "typedef struct {\n");
  hw_raw(hw, "  const char* message;\n");
  hw_raw(hw, "  ParseErrorType type;\n");
  hw_raw(hw, "  int32_t cp_offset;\n");
  hw_raw(hw, "  int32_t cp_size;\n");
  hw_raw(hw, "} ParseError;\n");
  hw_raw(hw, "typedef ParseError* ParseErrors;\n");
  hw_blank(hw);

  // PegRef
  hw_raw(hw, "typedef struct {\n");
  hw_raw(hw, "  TokenChunk* tc;\n");
  hw_raw(hw, "  int32_t col;\n");
  hw_raw(hw, "  int32_t next_col;\n");
  hw_raw(hw, "} PegRef;\n");
  hw_blank(hw);

  // ParseResult
  hw_raw(hw, "typedef struct {\n");
  hw_raw(hw, "  PegRef main;\n");
  hw_raw(hw, "  TokenTree* tt;\n");
  hw_raw(hw, "  ParseErrors errors;\n");
  hw_raw(hw, "} ParseResult;\n");
  hw_blank(hw);

  // LexHook typedef + ParseContext
  hw_raw(hw, "typedef int32_t (*LexHook)(void* userdata, Token* token, const char* token_str_start);\n");
  hw_blank(hw);
  hw_raw(hw, "typedef struct {\n");
  hw_raw(hw, "  void* userdata;\n");
  for (int32_t i = HOOK_ID_BUILTIN_COUNT; i < n_hooks + input->hooks.start_num; i++) {
    const char* name = symtab_get(&input->hooks, i);
    hw_fmt(hw, "  LexHook %s;\n", name + 1);
  }
  hw_raw(hw, "} ParseContext;\n");
  hw_blank(hw);

  // extern declarations
  hw_raw(hw, "extern void vpa_lex(void* src, int32_t len, void* tt, void* errors, void* ctx);\n");
  hw_blank(hw);

  if (prefix) {
    hw_fmt(hw, "extern ParseResult %s_parse(ParseContext lc, char* src);\n", prefix);
    hw_fmt(hw, "extern void %s_cleanup(ParseResult r);\n", prefix);
    hw_blank(hw);
  }
}

// --- Main entry point ---

void vpa_gen(VpaGenInput* input, HeaderWriter* hw, IrWriter* w, const char* prefix) {
  int32_t n_scopes = (int32_t)darray_size(input->scopes);

  Actions actions = darray_new(sizeof(Action), 0);
  Action sentinel = {.action_id = 0, .action_units = NULL};
  darray_push(actions, sentinel);

  for (int32_t i = 0; i < n_scopes; i++) {
    _gen_scope_dfa(input, w, &input->scopes[i], &actions);
  }

  _gen_dispatch(input, w, actions);
  _gen_vpa_lex(input, w);
  _gen_parse_entry(w, prefix);

  int32_t n_actions = (int32_t)darray_size(actions);
  _gen_header(input, hw, n_actions, prefix);

  darray_del(actions);
}
