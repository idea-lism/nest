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
  const char* end_scope_name;  // if action contains .end, scope name for parse_{name}
  int32_t begin_scope_id;      // if action contains .begin, target scope_id; -1 otherwise
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

static int32_t _intern_action(Actions* actions, VpaActionUnits au, const char* end_scope_name) {
  if (!au || darray_size(au) == 0) {
    return 0;
  }
  int32_t n = (int32_t)darray_size(*actions);
  for (int32_t i = 1; i < n; i++) {
    if (_au_equal((*actions)[i].action_units, au)) {
      return i;
    }
  }
  Action a = {.action_id = n, .action_units = au, .end_scope_name = end_scope_name, .begin_scope_id = -1};
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

  Aut* aut = aut_new(func_name, input->source_file_name);
  Re* re = re_new(aut);

  bool started = false;

  re_lparen(re);

  for (size_t i = 0; i < darray_size(scope->children); i++) {
    VpaUnit* u = &scope->children[i];
    // For VPA_CALL, use the called scope's leader actions (spec: "Sub-scope calls inlines the leader regexp")
    VpaActionUnits action_au = u->action_units;
    int32_t begin_scope_id = -1;
    if (u->kind == VPA_CALL) {
      for (int32_t s = 0; s < (int32_t)darray_size(input->scopes); s++) {
        if (input->scopes[s].scope_id == u->call_scope_id) {
          if (input->scopes[s].leader.action_units) {
            action_au = input->scopes[s].leader.action_units;
          }
          begin_scope_id = input->scopes[s].scope_id;
          break;
        }
      }
    }
    // check action_au for .end hook
    const char* end_name = NULL;
    if (action_au) {
      int32_t n_au = (int32_t)darray_size(action_au);
      for (int32_t j = 0; j < n_au; j++) {
        if (action_au[j] <= 0 && -action_au[j] == HOOK_ID_END) {
          end_name = scope->name;
          break;
        }
      }
    }
    int32_t aid = _intern_action(actions, action_au, end_name);
    // store begin_scope_id on the action if it contains .begin
    if (aid > 0 && begin_scope_id >= 0) {
      (*actions)[aid].begin_scope_id = begin_scope_id;
    }

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

  irwriter_define_startf(w, wrapper_name, "{i64, i64} @%s(i64 %%state, i64 %%cp)", wrapper_name);
  irwriter_bb(w);
  irwriter_dbg(w, scope->source_line, scope->source_col);

  // _dfa_* functions use native i64 ABI
  IrVal result = irwriter_call_retf(w, "{i64, i64}", func_name, "i64 %%state, i64 %%cp");
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
  irwriter_declare(w, "i8*", "tt_pop", "i8*, i32");

  // pre-declare PEG parse functions for scopes with .end
  for (int32_t i = 1; i < n_actions; i++) {
    if (actions[i].end_scope_name) {
      int fn_len = snprintf(NULL, 0, "parse_%s", actions[i].end_scope_name);
      char* fn_name = malloc((size_t)fn_len + 1);
      snprintf(fn_name, (size_t)fn_len + 1, "parse_%s", actions[i].end_scope_name);
      irwriter_declare(w, "{i64, i64}", fn_name, "ptr, ptr");
      free(fn_name);
    }
  }

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
          irwriter_call_retf(w, "i8*", "tt_push_assoc", "i8* %%tt, i32 %d", actions[i].begin_scope_id);
        } else if (hook_id == HOOK_ID_END) {
          // invoke PEG parser before popping scope
          if (actions[i].end_scope_name) {
            int fn_len = snprintf(NULL, 0, "parse_%s", actions[i].end_scope_name);
            char* fn_name = malloc((size_t)fn_len + 1);
            snprintf(fn_name, (size_t)fn_len + 1, "parse_%s", actions[i].end_scope_name);
            IrVal stack_buf = (IrVal)(irwriter_next_reg(w));
            irwriter_rawf(w, "  %%r%d = alloca i64, i32 1024\n", (int)stack_buf);
            irwriter_call_retf(w, "{i64, i64}", fn_name, "ptr %%tt, ptr %%r%d", (int)stack_buf);
            free(fn_name);
          }
          // pop scope + add scope-ref to parent
          IrVal cp_end = irwriter_binop(w, "add", "i32", irwriter_imm(w, "%cp_start"), irwriter_imm(w, "%cp_size"));
          irwriter_call_retf(w, "i8*", "tt_pop", "i8* %%tt, i32 %%r%d", (int)cp_end);
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
  irwriter_declare(w, "void", "tt_mark_newline", "i8*, i32");

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
  // --- set root chunk scope_id to main scope ---
  if (n_scopes > 0) {
    // tt->current is at byte offset 24 in TokenTree struct (after src, newline_map, root)
    // Actually: TokenTree = {src(ptr), newline_map(ptr), root(ptr), current(ptr), table(ptr)}
    // On 64-bit: current is at offset 24 bytes
    irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%tt, i64 24\n", irwriter_next_reg(w));
    IrVal current_ptr_ptr = (IrVal)(irwriter_next_reg(w) - 1);
    IrVal current_chunk = irwriter_load(w, "ptr", current_ptr_ptr);
    irwriter_store(w, "i32", irwriter_imm_int(w, input->scopes[0].scope_id), current_chunk);
    irwriter_comment(w, "root scope_id = %d", input->scopes[0].scope_id);
    // set root chunk aux_value = tt (for PEG loaders to find TokenTree)
    irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%r%d, i64 16\n", irwriter_next_reg(w), (int)current_chunk);
    IrVal aux_ptr = (IrVal)(irwriter_next_reg(w) - 1);
    irwriter_store(w, "ptr", irwriter_imm(w, "%tt"), aux_ptr);
  }
  IrLabel loop_bb = irwriter_label(w);
  IrLabel read_bb = irwriter_label(w);
  IrLabel dispatch_bb = irwriter_label(w);
  IrLabel dispatch_action_bb = irwriter_label(w);
  IrLabel done_bb = irwriter_label(w);
  irwriter_br(w, loop_bb);
  // --- loop: check if at end ---
  irwriter_bb_at(w, loop_bb);
  irwriter_comment(w, "loop: check if at end");
  IrVal cp_off = irwriter_load(w, "i32", cp_off_ptr);
  IrVal at_end = irwriter_icmp(w, "sge", "i32", cp_off, irwriter_imm(w, "%len"));
  irwriter_br_cond(w, at_end, dispatch_bb, read_bb);

  // --- read cp and run scope-switched DFA ---
  irwriter_bb_at(w, read_bb);
  IrVal cp_off2 = irwriter_load(w, "i32", cp_off_ptr);
  IrVal cp = irwriter_call_retf(w, "i32", read_cp_name, "i8* %%src, i32 %%r%d", (int)cp_off2);

  // mark newline in bitmap
  IrLabel nl_bb = irwriter_label(w);
  IrLabel after_nl_bb = irwriter_label(w);
  IrVal is_nl = irwriter_icmp(w, "eq", "i32", cp, irwriter_imm_int(w, 10));
  irwriter_br_cond(w, is_nl, nl_bb, after_nl_bb);
  irwriter_bb_at(w, nl_bb);
  irwriter_call_void_fmtf(w, "tt_mark_newline", "i8* %%tt, i32 %%r%d", (int)cp_off2);
  irwriter_br(w, after_nl_bb);
  irwriter_bb_at(w, after_nl_bb);
  IrVal dfa_state = irwriter_load(w, "i32", dfa_state_ptr);

  if (n_scopes > 0) {
    IrVal state64 = irwriter_sext(w, "i32", dfa_state, "i64");
    IrVal cp64 = irwriter_sext(w, "i32", cp, "i64");
    // read tt->current->scope_id to decide which DFA to call
    irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%tt, i64 24\n", irwriter_next_reg(w));
    IrVal cur_ptr = irwriter_load(w, "ptr", (IrVal)(irwriter_next_reg(w) - 1));
    IrVal scope_id = irwriter_load(w, "i32", cur_ptr);

    // after_dfa merges results via phi
    IrLabel after_dfa = irwriter_label(w);
    IrLabel* scope_bbs = malloc((size_t)n_scopes * sizeof(IrLabel));
    IrVal* scope_results = malloc((size_t)n_scopes * sizeof(IrVal));

    // switch on scope_id to call the right lex_{scope}
    irwriter_comment(w, "scope-switch: dispatch to scope DFA");
    irwriter_switch_start(w, "i32", scope_id, done_bb);
    for (int32_t si = 0; si < n_scopes; si++) {
      scope_bbs[si] = irwriter_label(w);
      irwriter_switch_case(w, "i32", input->scopes[si].scope_id, scope_bbs[si]);
    }
    irwriter_switch_end(w);

    // one basic block per scope calling its lex_{name}
    for (int32_t si = 0; si < n_scopes; si++) {
      irwriter_bb_at(w, scope_bbs[si]);
      char* lex_fn = NULL;
      int lex_fn_len = snprintf(NULL, 0, "lex_%s", input->scopes[si].name);
      lex_fn = malloc((size_t)lex_fn_len + 1);
      snprintf(lex_fn, (size_t)lex_fn_len + 1, "lex_%s", input->scopes[si].name);
      scope_results[si] = irwriter_call_retf(w, "{i64, i64}", lex_fn, "i64 %%r%d, i64 %%r%d", (int)state64, (int)cp64);
      free(lex_fn);
      irwriter_br(w, after_dfa);
    }

    // merge with phi
    irwriter_bb_at(w, after_dfa);
    int phi_reg = irwriter_next_reg(w);
    irwriter_rawf(w, "  %%r%d = phi {i64, i64} ", phi_reg);
    for (int32_t si = 0; si < n_scopes; si++) {
      if (si > 0) {
        irwriter_rawf(w, ", ");
      }
      irwriter_rawf(w, "[%%r%d, %%L%d]", (int)scope_results[si], (int)scope_bbs[si]);
    }
    irwriter_rawf(w, "\n");
    IrVal result = (IrVal)phi_reg;

    free(scope_bbs);
    free(scope_results);
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
    irwriter_comment(w, "has_action: check if positive");
    IrLabel action_pos_bb = irwriter_label(w);
    IrLabel advance_bb = irwriter_label(w);
    IrVal action_is_pos = irwriter_icmp(w, "sgt", "i32", action, irwriter_imm_int(w, 0));
    irwriter_br_cond(w, action_is_pos, action_pos_bb, advance_bb);
    irwriter_bb_at(w, action_pos_bb);
    irwriter_comment(w, "record last_action + last_match_off");
    irwriter_store(w, "i32", action, last_action_ptr);
    IrVal next_off = irwriter_binop(w, "add", "i32", cp_off2, irwriter_imm_int(w, 1));
    irwriter_store(w, "i32", next_off, last_match_off_ptr);
    irwriter_br(w, advance_bb);
    irwriter_bb_at(w, advance_bb);
    irwriter_comment(w, "advance cp_off, store new_state");
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

static void _gen_parse_entry(IrWriter* w, const char* prefix, int32_t main_rule_row) {
  if (!prefix) {
    return;
  }

  // vpa_lex is already defined in this module; no declare needed
  irwriter_declare(w, "i8*", "tt_tree_new", "i8*");
  irwriter_declare(w, "i32", "ustr_size", "i8*");
  irwriter_declare(w, "{i64, i64}", "parse_main", "ptr, ptr");
  irwriter_declare(w, "ptr", "tt_current", "ptr");
  // {prefix}_parse — use sret for ABI compatibility with C
  int parse_name_len = snprintf(NULL, 0, "%s_parse", prefix);
  char* parse_name = malloc((size_t)parse_name_len + 1);
  snprintf(parse_name, (size_t)parse_name_len + 1, "%s_parse", prefix);
  irwriter_define_startf(w, parse_name,
                         "void @%s(ptr noalias sret(" PARSE_RESULT_TY ") align 8 %%retval, i64 %%ctx_i64, ptr %%src)",
                         parse_name);
  free(parse_name);
  irwriter_bb(w);
  irwriter_dbg(w, 0, 0);
  IrVal len = irwriter_call_retf(w, "i32", "ustr_size", "ptr %%src");
  IrVal len64 = irwriter_sext(w, "i32", len, "i64");
  IrVal tt = irwriter_call_retf(w, "ptr", "tt_tree_new", "ptr %%src");
  irwriter_rawf(w, "  %%r%d = inttoptr i64 %%ctx_i64 to ptr\n", irwriter_next_reg(w));
  IrVal ctx_ptr = (IrVal)(irwriter_next_reg(w) - 1);
  irwriter_call_void_fmtf(w, "vpa_lex", "ptr %%src, i64 %%r%d, ptr %%r%d, ptr null, ptr %%r%d", (int)len64, (int)tt,
                          (int)ctx_ptr);
  // vpa_lex sets root chunk scope_id; now call parse_main
  // call parse_main on the root scope
  IrVal stack_buf = (IrVal)(irwriter_next_reg(w));
  irwriter_rawf(w, "  %%r%d = alloca i64, i32 1024\n", (int)stack_buf);
  irwriter_call_retf(w, "{i64, i64}", "parse_main", "ptr %%r%d, ptr %%r%d", (int)tt, (int)stack_buf);
  IrVal tc = irwriter_call_retf(w, "ptr", "tt_current", "ptr %%r%d", (int)tt);
  // store fields to sret pointer: {PegRef.tc, PegRef.col, PegRef.row, tt, errors}
  // field 0: PegRef.tc (ptr) at offset 0
  irwriter_rawf(w, "  %%r%d = getelementptr inbounds " PARSE_RESULT_TY ", ptr %%retval, i32 0, i32 0\n",
                irwriter_next_reg(w));
  IrVal f0 = (IrVal)(irwriter_next_reg(w) - 1);
  irwriter_store(w, "ptr", tc, f0);
  // field 1: PegRef.col (i64) at offset 1
  irwriter_rawf(w, "  %%r%d = getelementptr inbounds " PARSE_RESULT_TY ", ptr %%retval, i32 0, i32 1\n",
                irwriter_next_reg(w));
  IrVal f1 = (IrVal)(irwriter_next_reg(w) - 1);
  irwriter_store(w, "i64", irwriter_imm_int(w, 0), f1);
  // field 2: PegRef.row (i64) at offset 2
  irwriter_rawf(w, "  %%r%d = getelementptr inbounds " PARSE_RESULT_TY ", ptr %%retval, i32 0, i32 2\n",
                irwriter_next_reg(w));
  IrVal f2 = (IrVal)(irwriter_next_reg(w) - 1);
  irwriter_store(w, "i64", irwriter_imm_int(w, main_rule_row), f2);
  // field 3: TokenTree* tt
  irwriter_rawf(w, "  %%r%d = getelementptr inbounds " PARSE_RESULT_TY ", ptr %%retval, i32 0, i32 3\n",
                irwriter_next_reg(w));
  IrVal f3 = (IrVal)(irwriter_next_reg(w) - 1);
  irwriter_store(w, "ptr", tt, f3);
  // field 4: ParseErrors (null)
  irwriter_rawf(w, "  %%r%d = getelementptr inbounds " PARSE_RESULT_TY ", ptr %%retval, i32 0, i32 4\n",
                irwriter_next_reg(w));
  IrVal f4 = (IrVal)(irwriter_next_reg(w) - 1);
  irwriter_store(w, "ptr", irwriter_imm(w, "null"), f4);
  irwriter_ret_void(w);
  irwriter_define_end(w);

  // {prefix}_cleanup(result) -> void
  int cleanup_name_len = snprintf(NULL, 0, "%s_cleanup", prefix);
  char* cleanup_name = malloc((size_t)cleanup_name_len + 1);
  snprintf(cleanup_name, (size_t)cleanup_name_len + 1, "%s_cleanup", prefix);
  irwriter_define_startf(w, cleanup_name, "void @%s(ptr %%res)", cleanup_name);
  free(cleanup_name);
  irwriter_bb(w);
  irwriter_dbg(w, 0, 0);
  irwriter_ret_void(w);
  irwriter_define_end(w);
}

// --- Header generation ---

static void _print_upper(HeaderWriter* hw, const char* s) {
  for (; *s; s++) {
    hdwriter_putc(hw, (char)toupper((unsigned char)*s));
  }
}

static void _gen_header(VpaGenInput* input, HeaderWriter* hw, const char* prefix) {
  hdwriter_puts(hw, "#pragma once\n");
  hdwriter_putc(hw, '\n');
  hdwriter_puts(hw, "#include <stdbool.h>\n");
  hdwriter_putc(hw, '\n');

  // inline amalgamated runtime
  hdwriter_puts(hw, (const char*)NEST_RT);
  hdwriter_putc(hw, '\n');

  int32_t n_scopes = (int32_t)darray_size(input->scopes);

  // scope defines
  for (int32_t i = 0; i < n_scopes; i++) {
    hdwriter_puts(hw, "#define SCOPE_");
    _print_upper(hw, input->scopes[i].name);
    hdwriter_printf(hw, " %d\n", input->scopes[i].scope_id);
  }
  hdwriter_putc(hw, '\n');

  // token defines — skip keyword literals ("lit." prefix)
  int32_t n_tokens = symtab_count(&input->tokens);
  for (int32_t i = 0; i < n_tokens; i++) {
    int32_t tok_id = i + input->tokens.start_num;
    const char* name = symtab_get(&input->tokens, tok_id);
    if (strncmp(name, "@lit.", 5) == 0) {
      continue;
    }
    hdwriter_puts(hw, "#define TOK_");
    _print_upper(hw, name + 1);
    hdwriter_printf(hw, " %d\n", tok_id);
  }
  hdwriter_putc(hw, '\n');

  // builtin hook ID defines (only user-facing ones)
  hdwriter_printf(hw, "#define HOOK_FAIL %d\n", -HOOK_ID_FAIL);
  hdwriter_printf(hw, "#define HOOK_UNPARSE %d\n", -HOOK_ID_UNPARSE);
  hdwriter_putc(hw, '\n');

  int32_t n_hooks = symtab_count(&input->hooks);

  // ParseErrorType enum
  hdwriter_puts(hw, "typedef enum {\n");
  hdwriter_puts(hw, "  PARSE_ERROR_INVALID_HOOK,\n");
  hdwriter_puts(hw, "  PARSE_ERROR_REQUIRE_MORE_INPUT,\n");
  hdwriter_puts(hw, "  PARSE_ERROR_TOKEN_ERR,\n");
  hdwriter_puts(hw, "  PARSE_ERROR_INVALID_SYNTAX,\n");
  hdwriter_puts(hw, "} ParseErrorType;\n");
  hdwriter_putc(hw, '\n');

  // ParseError
  hdwriter_puts(hw, "typedef struct {\n");
  hdwriter_puts(hw, "  const char* message;\n");
  hdwriter_puts(hw, "  ParseErrorType type;\n");
  hdwriter_puts(hw, "  int32_t cp_offset;\n");
  hdwriter_puts(hw, "  int32_t cp_size;\n");
  hdwriter_puts(hw, "} ParseError;\n");
  hdwriter_puts(hw, "typedef ParseError* ParseErrors;\n");
  hdwriter_putc(hw, '\n');

  // PegRef — already defined by PEG header when PEG rules are present; guarded to avoid redefinition.
  hdwriter_puts(hw, "#ifndef _NEST_PEGREF\n#define _NEST_PEGREF\n");
  hdwriter_puts(hw, "typedef struct {\n  TokenChunk* tc;\n  int64_t col;\n  int64_t row;\n} PegRef;\n");
  hdwriter_puts(hw, "#endif\n");
  hdwriter_putc(hw, '\n');

  // ParseResult
  hdwriter_puts(hw, "typedef struct {\n");
  hdwriter_puts(hw, "  PegRef main;\n");
  hdwriter_puts(hw, "  TokenTree* tt;\n");
  hdwriter_puts(hw, "  ParseErrors errors;\n");
  hdwriter_puts(hw, "} ParseResult;\n");
  hdwriter_putc(hw, '\n');

  // LexHook typedef + ParseContext
  hdwriter_puts(hw, "typedef int32_t (*LexHook)(void* userdata, Token* token, const char* token_str_start);\n");
  hdwriter_putc(hw, '\n');
  hdwriter_puts(hw, "typedef struct {\n");
  hdwriter_puts(hw, "  void* userdata;\n");
  for (int32_t i = HOOK_ID_BUILTIN_COUNT; i < n_hooks + input->hooks.start_num; i++) {
    const char* name = symtab_get(&input->hooks, i);
    hdwriter_printf(hw, "  LexHook %s;\n", name + 1);
  }
  hdwriter_puts(hw, "} ParseContext;\n");
  hdwriter_putc(hw, '\n');

  // extern declarations
  hdwriter_puts(hw, "extern void vpa_lex(void* src, int64_t len, void* tt, void* errors, void* ctx);\n");
  hdwriter_putc(hw, '\n');

  if (prefix) {
    hdwriter_printf(hw, "extern ParseResult %s_parse(ParseContext $parse_context, char* src);\n", prefix);
    hdwriter_printf(hw, "extern void %s_cleanup(ParseResult r);\n", prefix);
    hdwriter_putc(hw, '\n');
  }
}

// --- Main entry point ---

void vpa_gen(VpaGenInput* input, HeaderWriter* hw, IrWriter* w, const char* prefix, int32_t main_rule_row) {
  int32_t n_scopes = (int32_t)darray_size(input->scopes);

  Actions actions = darray_new(sizeof(Action), 0);
  Action sentinel = {.action_id = 0, .action_units = NULL, .end_scope_name = NULL, .begin_scope_id = -1};
  darray_push(actions, sentinel);

  for (int32_t i = 0; i < n_scopes; i++) {
    _gen_scope_dfa(input, w, &input->scopes[i], &actions);
  }

  _gen_dispatch(input, w, actions);
  _gen_vpa_lex(input, w, prefix);
  _gen_parse_entry(w, prefix, main_rule_row);
  _gen_header(input, hw, prefix);

  darray_del(actions);
}
