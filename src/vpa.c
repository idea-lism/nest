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
  int func_name_len = snprintf(NULL, 0, "_dfa_%s", scope->name);
  char* func_name = malloc((size_t)func_name_len + 1);
  snprintf(func_name, (size_t)func_name_len + 1, "_dfa_%s", scope->name);

  Aut* aut = aut_new(func_name, "vpa");
  Re* re = re_new(aut);

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

    DebugInfo di = {u->source_line, u->source_col};
    re_ir_exec(re, ir, di);
    re_action(re, aid);
  }

  re_rparen(re);
  aut_optimize(aut);
  aut_gen_dfa(aut, w, false);

  // widened wrapper: lex_{name}(i32, i32) -> {i64, i64}
  int wrapper_name_len = snprintf(NULL, 0, "lex_%s", scope->name);
  char* wrapper_name = malloc((size_t)wrapper_name_len + 1);
  snprintf(wrapper_name, (size_t)wrapper_name_len + 1, "lex_%s", scope->name);

  irwriter_define_startf(w, wrapper_name, "{i64, i64} @%s(i64 %%state_i64, i64 %%cp_i64)", wrapper_name);
  irwriter_bb(w);
  irwriter_dbg(w, scope->source_line, scope->source_col);
  irwriter_rawf(w, "  %%state = trunc i64 %%state_i64 to i32\n");
  irwriter_rawf(w, "  %%cp = trunc i64 %%cp_i64 to i32\n");

  // _dfa_* functions use the widened {i64,i64}(i64,i64) ABI
  IrVal state64 = irwriter_sext(w, "i32", irwriter_imm(w, "%state"), "i64");
  IrVal cp64 = irwriter_sext(w, "i32", irwriter_imm(w, "%cp"), "i64");
  IrVal result = irwriter_call_retf(w, "{i64, i64}", func_name, "i64 %%r%d, i64 %%r%d", (int)state64, (int)cp64);
  irwriter_ret(w, "{i64, i64}", result);
  irwriter_define_end(w);

  re_del(re);
  aut_del(aut);
  free(func_name);
  free(wrapper_name);
}

// --- Dispatch function ---

static void _gen_dispatch(VpaGenInput* input, IrWriter* w, Actions actions) {
  int32_t n_actions = (int32_t)darray_size(actions);

  irwriter_declare(w, "void", "tt_add", "i8*, i32, i32, i32, i32");
  irwriter_declare(w, "i8*", "tt_push_assoc", "i8*, i32");
  irwriter_declare(w, "i8*", "tt_pop", "i8*");

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
          int fn_name_len = snprintf(NULL, 0, "vpa_hook_%s", hook_name + 1);
          char* fn_name = malloc((size_t)fn_name_len + 1);
          snprintf(fn_name, (size_t)fn_name_len + 1, "vpa_hook_%s", hook_name + 1);
          irwriter_declare(w, "i32", fn_name, "i8*, i8*, i8*");
          free(fn_name);
        }
      }
    }
  }

  irwriter_define_startf(
      w, "vpa_dispatch",
      "void @vpa_dispatch(i64 %%action_id_i64, i8* %%tt, i64 %%cp_start_i64, i64 %%cp_size_i64, i8* %%ctx)");
  irwriter_bb(w);
  irwriter_dbg(w, 0, 0);
  irwriter_rawf(w, "  %%action_id = trunc i64 %%action_id_i64 to i32\n");
  irwriter_rawf(w, "  %%cp_start = trunc i64 %%cp_start_i64 to i32\n");
  irwriter_rawf(w, "  %%cp_size = trunc i64 %%cp_size_i64 to i32\n");

  if (n_actions <= 1) {
    irwriter_ret_void(w);
    irwriter_define_end(w);
    return;
  }

  IrLabel default_bb = irwriter_label(w);
  IrLabel* case_bbs = malloc((size_t)n_actions * sizeof(IrLabel));
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
          irwriter_call_retf(w, "i8*", "tt_push_assoc", "i8* %%tt, i32 0");
        } else if (hook_id == HOOK_ID_END) {
          // pop scope from VPA stack
          irwriter_call_retf(w, "i8*", "tt_pop", "i8* %%tt");
        } else if (hook_id >= HOOK_ID_BUILTIN_COUNT) {
          // user hook
          const char* hook_name = symtab_get(&input->hooks, hook_id);
          int fn_name_len = snprintf(NULL, 0, "vpa_hook_%s", hook_name + 1);
          char* fn_name = malloc((size_t)fn_name_len + 1);
          snprintf(fn_name, (size_t)fn_name_len + 1, "vpa_hook_%s", hook_name + 1);
          IrVal ret_val = irwriter_call_retf(w, "i32", fn_name, "i8* %%ctx, i8* null, i8* null");
          free(fn_name);

          // if this hook has %effect, validate the return value
          EffectDecl* ed = _find_effect(input, hook_id);
          if (ed) {
            int32_t n_effects = (int32_t)darray_size(ed->effects);
            // build a chain: check ret_val against each valid effect, if none match -> error
            IrLabel ok_bb = irwriter_label(w);
            IrLabel err_bb = irwriter_label(w);
            IrLabel* effect_bbs = NULL;
            if (n_effects > 0) {
              effect_bbs = malloc((size_t)n_effects * sizeof(IrLabel));
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

            // error: invalid return from hook (TODO: error reporting)
            irwriter_bb_at(w, err_bb);
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

static void _gen_vpa_lex(VpaGenInput* input, IrWriter* w, const char* prefix) {
  int32_t n_scopes = (int32_t)darray_size(input->scopes);

  char* read_cp_name;
  if (prefix) {
    int read_cp_len = snprintf(NULL, 0, "%s_next_cp", prefix);
    read_cp_name = malloc((size_t)read_cp_len + 1);
    snprintf(read_cp_name, (size_t)read_cp_len + 1, "%s_next_cp", prefix);
  } else {
    read_cp_name = strdup("vpa_rt_read_cp");
  }
  irwriter_declare(w, "i32", read_cp_name, "i8*, i32");
  irwriter_declare(w, "i32", "tt_depth", "i8*");
  irwriter_declare(w, "void", "tt_add", "i8*, i32, i32, i32, i32");

  irwriter_define_startf(w, "vpa_lex", "void @vpa_lex(i8* %%src, i64 %%len_i64, i8* %%tt, i8* %%errors, i8* %%ctx)");

  irwriter_bb(w);
  irwriter_dbg(w, 0, 0);
  irwriter_rawf(w, "  %%len = trunc i64 %%len_i64 to i32\n");

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

  IrLabel loop_bb = irwriter_label(w);
  IrLabel read_bb = irwriter_label(w);
  IrLabel dispatch_bb = irwriter_label(w);
  IrLabel dispatch_action_bb = irwriter_label(w);
  IrLabel done_bb = irwriter_label(w);

  irwriter_br(w, loop_bb);

  // --- loop: check if at end ---
  irwriter_bb_at(w, loop_bb);
  IrVal cp_off = irwriter_load(w, "i32", cp_off_ptr);
  IrVal at_end = irwriter_icmp(w, "sge", "i32", cp_off, irwriter_imm(w, "%len"));
  irwriter_br_cond(w, at_end, dispatch_bb, read_bb);

  // --- read cp and run DFA ---
  irwriter_bb_at(w, read_bb);
  IrVal cp_off2 = irwriter_load(w, "i32", cp_off_ptr);
  IrVal cp = irwriter_call_retf(w, "i32", read_cp_name, "i8* %%src, i32 %%r%d", (int)cp_off2);
  IrVal dfa_state = irwriter_load(w, "i32", dfa_state_ptr);

  if (n_scopes > 0) {
    int dfa_fn_len = snprintf(NULL, 0, "_dfa_%s", input->scopes[0].name);
    char* dfa_fn = malloc((size_t)dfa_fn_len + 1);
    snprintf(dfa_fn, (size_t)dfa_fn_len + 1, "_dfa_%s", input->scopes[0].name);
    // DFA functions use {i64,i64}(i64,i64) ABI — widen i32 args and narrow results
    IrVal state64 = irwriter_sext(w, "i32", dfa_state, "i64");
    IrVal cp64 = irwriter_sext(w, "i32", cp, "i64");
    IrVal result = irwriter_call_retf(w, "{i64, i64}", dfa_fn, "i64 %%r%d, i64 %%r%d", (int)state64, (int)cp64);
    free(dfa_fn);
    IrVal new_state64 = irwriter_extractvalue(w, "{i64, i64}", result, 0);
    IrVal action64 = irwriter_extractvalue(w, "{i64, i64}", result, 1);
    irwriter_rawf(w, "  %%r%d = trunc i64 %%r%d to i32\n", irwriter_next_reg(w), (int)new_state64);
    IrVal new_state = (IrVal)(irwriter_next_reg(w) - 1);
    irwriter_rawf(w, "  %%r%d = trunc i64 %%r%d to i32\n", irwriter_next_reg(w), (int)action64);
    IrVal action = (IrVal)(irwriter_next_reg(w) - 1);

    IrLabel has_action_bb = irwriter_label(w);
    IrLabel no_action_bb = irwriter_label(w);
    IrVal action_valid = irwriter_icmp(w, "ne", "i32", action, irwriter_imm_int(w, -2));
    irwriter_br_cond(w, action_valid, has_action_bb, no_action_bb);

    irwriter_bb_at(w, has_action_bb);
    IrLabel action_pos_bb = irwriter_label(w);
    IrLabel advance_bb = irwriter_label(w);
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
  // vpa_dispatch uses i64 ABI — widen i32 args
  IrVal la64 = irwriter_sext(w, "i32", last_action, "i64");
  IrVal ts64 = irwriter_sext(w, "i32", tok_start, "i64");
  IrVal sz64 = irwriter_sext(w, "i32", tok_size, "i64");
  irwriter_call_void_fmtf(w, "vpa_dispatch", "i64 %%r%d, i8* %%tt, i64 %%r%d, i64 %%r%d, i8* %%ctx", (int)la64,
                          (int)ts64, (int)sz64);

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
  free(read_cp_name);
}

// --- {prefix}_parse / {prefix}_cleanup ---

// ParseResult = {PegRef, TokenTree*, ParseErrors} = {i8*, i32, i32, i8*, i8*}
#define PARSE_RESULT_TY "{i8*, i64, i64, i8*, i8*}"

static void _gen_parse_entry(IrWriter* w, const char* prefix) {
  if (!prefix) {
    return;
  }

  // vpa_lex is already defined in this module; no declare needed
  irwriter_declare(w, "i8*", "tt_tree_new", "i8*");
  irwriter_declare(w, "i32", "ustr_size", "i8*");

  // {prefix}_parse(ctx_ptr, src) -> ParseResult
  int parse_name_len = snprintf(NULL, 0, "%s_parse", prefix);
  char* parse_name = malloc((size_t)parse_name_len + 1);
  snprintf(parse_name, (size_t)parse_name_len + 1, "%s_parse", prefix);
  irwriter_define_startf(w, parse_name, PARSE_RESULT_TY " @%s(i8* %%ctx, i8* %%src)", parse_name);
  free(parse_name);
  irwriter_bb(w);
  irwriter_dbg(w, 0, 0);
  IrVal len = irwriter_call_retf(w, "i32", "ustr_size", "i8* %%src");
  IrVal len64 = irwriter_sext(w, "i32", len, "i64");
  IrVal tt = irwriter_call_retf(w, "i8*", "tt_tree_new", "i8* %%src");
  irwriter_call_void_fmtf(w, "vpa_lex", "i8* %%src, i64 %%r%d, i8* %%r%d, i8* null, i8* %%ctx", (int)len64, (int)tt);

  IrVal r0 = irwriter_insertvalue(w, PARSE_RESULT_TY, -1, "i8*", irwriter_imm(w, "null"), 0);
  IrVal r1 = irwriter_insertvalue(w, PARSE_RESULT_TY, r0, "i64", irwriter_imm_int(w, 0), 1);
  IrVal r2 = irwriter_insertvalue(w, PARSE_RESULT_TY, r1, "i64", irwriter_imm_int(w, 0), 2);
  IrVal r3 = irwriter_insertvalue(w, PARSE_RESULT_TY, r2, "i8*", tt, 3);
  IrVal r4 = irwriter_insertvalue(w, PARSE_RESULT_TY, r3, "i8*", irwriter_imm(w, "null"), 4);
  irwriter_ret(w, PARSE_RESULT_TY, r4);
  irwriter_define_end(w);

  // {prefix}_cleanup(result) -> void
  int cleanup_name_len = snprintf(NULL, 0, "%s_cleanup", prefix);
  char* cleanup_name = malloc((size_t)cleanup_name_len + 1);
  snprintf(cleanup_name, (size_t)cleanup_name_len + 1, "%s_cleanup", prefix);
  irwriter_define_startf(w, cleanup_name, "void @%s(" PARSE_RESULT_TY " %%res)", cleanup_name);
  free(cleanup_name);
  irwriter_bb(w);
  irwriter_dbg(w, 0, 0);
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
  hw_include_sys(hw, "stdbool.h");
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
    if (strncmp(name, "@lit.", 5) == 0) {
      continue;
    }
    hw_raw(hw, "#define TOK_");
    _hw_upper(hw, name + 1);
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

  // PegRef — already defined by PEG header when PEG rules are present; guarded to avoid redefinition.
  hw_raw(hw, "#ifndef _NEST_PEGREF\n#define _NEST_PEGREF\n");
  hw_raw(hw, "typedef struct {\n  TokenChunk* tc;\n  int64_t col;\n  int64_t row;\n} PegRef;\n");
  hw_raw(hw, "#endif\n");
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
  hw_raw(hw, "extern void vpa_lex(void* src, int64_t len, void* tt, void* errors, void* ctx);\n");
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
  _gen_vpa_lex(input, w, prefix);
  _gen_parse_entry(w, prefix);

  int32_t n_actions = (int32_t)darray_size(actions);
  _gen_header(input, hw, n_actions, prefix);

  darray_del(actions);
}
