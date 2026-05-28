#include "vpa.h"
#include "darray.h"
#include "re.h"
#include "re_ir.h"
#include "symtab.h"
#include "xmalloc.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// --- Actions table ---

typedef struct {
  int32_t action_id;
  VpaActionUnits action_units; // not owned
  int32_t begin_scope_id;      // if action contains .begin, target scope_id; -1 otherwise
} Action;

typedef Action* Actions;

static bool _au_equal(VpaActionUnits a, VpaActionUnits b) {
  if (!a && !b) return true;
  if (!a || !b) return false;
  size_t na = darray_size(a);
  size_t nb = darray_size(b);
  return na == nb && memcmp(a, b, na * sizeof(int32_t)) == 0;
}

static int32_t _intern_action(Actions* actions, VpaActionUnits au) {
  if (!au || darray_size(au) == 0) return 0;
  size_t n = darray_size(*actions);
  for (size_t i = 1; i < n; i++) {
    if (_au_equal((*actions)[i].action_units, au)) return (int32_t)i;
  }
  Action a = {.action_id = (int32_t)n, .action_units = au, .begin_scope_id = -1};
  darray_push(*actions, a);
  return (int32_t)n;
}

// --- DFA generation per scope ---

static void _gen_scope_dfa(VpaGenInput* input, IrWriter* w, VpaScope* scope, Actions* actions) {
  int func_name_len = snprintf(NULL, 0, "_dfa_%s", scope->name);
  char* func_name = XMALLOC((size_t)func_name_len + 1);
  snprintf(func_name, (size_t)func_name_len + 1, "_dfa_%s", scope->name);

  Aut* aut = aut_new(func_name, input->source_file_name);
  Re* re = re_new(aut);

  bool started = false;

  re_lparen(re);

  for (size_t i = 0; i < darray_size(scope->children); i++) {
    VpaUnit* u = &scope->children[i];
    // VPA_EOF is handled separately, not in DFA
    if (u->kind == VPA_EOF) {
      scope->eof_action = _intern_action(actions, u->action_units);
      continue;
    }
    // For VPA_CALL, use the called scope's leader actions (spec: "Sub-scope calls inlines the leader regexp")
    VpaActionUnits action_au = u->action_units;
    int32_t begin_scope_id = -1;
    if (u->kind == VPA_CALL) {
      for (size_t s = 0; s < darray_size(input->scopes); s++) {
        if (input->scopes[s].scope_id == u->call_scope_id) {
          if (input->scopes[s].leader.action_units) {
            action_au = input->scopes[s].leader.action_units;
          }
          begin_scope_id = input->scopes[s].scope_id;
          break;
        }
      }
    }
    int32_t aid = _intern_action(actions, action_au);
    if (aid > 0 && begin_scope_id >= 0) {
      (*actions)[aid].begin_scope_id = begin_scope_id;
    }

    ReIr ir = NULL;
    if (u->kind == VPA_RE) {
      ir = u->re;
    } else if (u->kind == VPA_CALL) {
      for (size_t s = 0; s < darray_size(input->scopes); s++) {
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

    re_ir_exec(re, ir, input->source_file_name, input->re_frags);
    re_action(re, aid);
  }

  re_rparen(re);
  aut_optimize(aut);
  aut_gen_dfa(aut, w, false);

  re_del(re);
  aut_del(aut);
  XFREE(func_name);
}

// --- Header generation ---

static void _print_upper(HeaderWriter* hw, const char* s) {
  for (; *s; s++) {
    hdwriter_putc(hw, (char)toupper((unsigned char)*s));
  }
}

static void _gen_init_lexers(VpaGenInput* input, IrWriter* w, const char* prefix) {
  int32_t n_scopes = (int32_t)darray_size(input->scopes);
  char* init_name = NULL;
  int init_len = snprintf(NULL, 0, "%s_init_lexers", prefix);
  init_name = XMALLOC((size_t)init_len + 1);
  snprintf(init_name, (size_t)init_len + 1, "%s_init_lexers", prefix);

  irwriter_define_startf(w, init_name, "void @%s(ptr %%lexer)", init_name);
  irwriter_bb(w);
  irwriter_dbg(w, 0, 0);
  for (int32_t i = 0; i < n_scopes; i++) {
    IrVal gep = irwriter_next_reg(w);
    irwriter_rawf(w, "  %%r%d = getelementptr {", (int)gep);
    for (int32_t j = 0; j < n_scopes; j++) {
      if (j > 0) {
        irwriter_raw(w, ", ");
      }
      irwriter_raw(w, "ptr");
    }
    irwriter_rawf(w, "}, ptr %%lexer, i32 0, i32 %d\n", i);
    irwriter_rawf(w, "  store ptr @_dfa_%s, ptr %%r%d\n", input->scopes[i].name, (int)gep);
  }
  irwriter_ret_void(w);
  irwriter_define_end(w);
  XFREE(init_name);
}

static bool _is_ignored_tok(VpaGenInput* input, int32_t tok_id) {
  if (!input->ignore_names.offsets) return false;
  const char* name = symtab_get(&input->tokens, tok_id);
  return name && symtab_find(&input->ignore_names, name) >= 0;
}

static void _gen_header(VpaGenInput* input, HeaderWriter* hw, const char* prefix, Actions actions) {
  // caller (nest.c) has already written #pragma once, includes, and runtime header

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
  hdwriter_printf(hw, "#define HOOK_NOOP %d\n", -HOOK_ID_NOOP);
  hdwriter_putc(hw, '\n');

  int32_t n_hooks = symtab_count(&input->hooks);

  hdwriter_printf(hw, "typedef struct { int64_t state; int64_t action_id; } %sAutResult;\n", prefix);
  hdwriter_printf(hw, "typedef %sAutResult (*%sAutFn)(int64_t state, int64_t cp);\n", prefix, prefix);
  hdwriter_puts(hw, "typedef struct {\n");
  for (int32_t i = 0; i < n_scopes; i++) {
    hdwriter_printf(hw, "  %sAutFn aut_%s;\n", prefix, input->scopes[i].name);
  }
  hdwriter_printf(hw, "} %sLexer;\n", prefix);
  hdwriter_printf(hw, "extern void %s_init_lexers(%sLexer* lexer);\n\n", prefix, prefix);

  // ParseErrorType, ParseError, ParseResult, PegRef, PegLink — all in nest_rt.h

  // LexHook typedef + ParseContext
  hdwriter_puts(hw, "typedef int32_t (*LexHook)(void* userdata, Token* token, const char* token_str_start);\n");
  hdwriter_putc(hw, '\n');
  hdwriter_puts(hw, "typedef struct {\n");
  hdwriter_puts(hw, "  void* user_data;\n");
  hdwriter_puts(hw, "  const char* source_file_name;\n");
  for (int32_t i = HOOK_ID_BUILTIN_COUNT; i < n_hooks + input->hooks.start_num; i++) {
    const char* name = symtab_get(&input->hooks, i);
    hdwriter_printf(hw, "  LexHook %s;\n", name + 1);
  }
  hdwriter_puts(hw, "} ParseContext;\n");
  hdwriter_putc(hw, '\n');

  hdwriter_printf(hw, "static inline %sAutFn %s_aut_for_scope(%sLexer* lexer, int32_t scope_id) {\n", prefix, prefix, prefix);
  hdwriter_puts(hw, "  switch (scope_id) {\n");
  for (int32_t i = 0; i < n_scopes; i++) {
    hdwriter_printf(hw, "  case %d: return lexer->aut_%s;\n", input->scopes[i].scope_id, input->scopes[i].name);
  }
  hdwriter_puts(hw, "  default: return NULL;\n  }\n}\n\n");

  hdwriter_printf(hw, "static inline int32_t %s_eof_action(int32_t scope_id) {\n", prefix);
  hdwriter_puts(hw, "  switch (scope_id) {\n");
  for (int32_t i = 0; i < n_scopes; i++) {
    if (input->scopes[i].eof_action > 0) {
      hdwriter_printf(hw, "  case %d: return %d;\n", input->scopes[i].scope_id, input->scopes[i].eof_action);
    }
  }
  hdwriter_puts(hw, "  default: return 0;\n  }\n}\n\n");

  hdwriter_printf(hw, "static inline int64_t %s_dispatch_action(ParseContext* ctx, TokenTree* tt, int32_t action_id, int32_t cp_start, int32_t cp_size) {\n", prefix);
  hdwriter_puts(hw, "  switch (action_id) {\n");
  for (size_t ai = 1; ai < darray_size(actions); ai++) {
    hdwriter_printf(hw, "  case %d:\n", (int32_t)ai);
    VpaActionUnits au = actions[ai].action_units;
    for (size_t j = 0; au && j < darray_size(au); j++) {
      int32_t auid = au[j];
      if (auid > 0) {
        if (!_is_ignored_tok(input, auid)) {
          hdwriter_printf(hw, "    tt_add(tt, %d, cp_start, cp_size, -1);\n", auid);
        }
      } else if (auid < 0) {
        int32_t hook_id = -auid;
        if (hook_id == HOOK_ID_BEGIN) {
          hdwriter_printf(hw, "    tt_push_assoc(tt, %d);\n", actions[ai].begin_scope_id);
        } else if (hook_id == HOOK_ID_END) {
          hdwriter_puts(hw, "    tt_pop(tt, cp_start + cp_size);\n");
        } else if (hook_id == HOOK_ID_FAIL) {
          hdwriter_puts(hw, "    return cp_start;\n");
        } else if (hook_id >= HOOK_ID_BUILTIN_COUNT) {
          const char* hook_name = symtab_get(&input->hooks, hook_id);
          hdwriter_printf(hw, "    if (ctx->%s) { int32_t $r = ctx->%s(ctx->user_data, NULL, NULL); if ($r == HOOK_FAIL) return cp_start; }\n", hook_name + 1, hook_name + 1);
        }
      }
    }
    hdwriter_puts(hw, "    return -1;\n");
  }
  hdwriter_puts(hw, "  default: return cp_start;\n  }\n}\n\n");

  hdwriter_printf(hw, "static inline int64_t %s_lex(ParseContext* ctx, %sLexer* lexer, const char* input, TokenTree* tt) {\n", prefix, prefix);
  hdwriter_puts(hw, "  UstrIter it;\n  ustr_iter_init(&it, input, 0);\n  int32_t len = ustr_size(input);\n  int32_t token_start = 0;\n");
  hdwriter_puts(hw, "  while (token_start < len) {\n");
  hdwriter_puts(hw, "    int32_t cp_off = token_start;\n    int64_t state = 0;\n    int32_t last_action = 0;\n    int32_t last_match = -1;\n    ustr_iter_seek(&it, token_start);\n");
  hdwriter_puts(hw, "    while (cp_off < len) {\n");
  hdwriter_printf(hw, "      %sAutFn aut = %s_aut_for_scope(lexer, tt->current->scope_id);\n", prefix, prefix);
  hdwriter_puts(hw, "      if (!aut) return cp_off;\n      int32_t cp = ustr_iter_next(&it);\n      if (cp == 10) tt_mark_newline(tt, cp_off);\n");
  hdwriter_printf(hw, "      %sAutResult r = aut(state, cp);\n", prefix);
  hdwriter_puts(hw, "      if (r.action_id == -2) break;\n      state = r.state;\n      cp_off++;\n      if (r.action_id >= 0) { last_action = (int32_t)r.action_id; last_match = cp_off; }\n    }\n");
  hdwriter_puts(hw, "    if (last_match < 0) return token_start;\n");
  hdwriter_printf(hw, "    if (last_action > 0) { int64_t err = %s_dispatch_action(ctx, tt, last_action, token_start, last_match - token_start); if (err >= 0) return err; }\n", prefix);
  hdwriter_puts(hw, "    token_start = last_match;\n  }\n");
  hdwriter_puts(hw, "  for (int32_t guard = 0; guard < 1024 && tt->current; guard++) {\n");
  hdwriter_printf(hw, "    int32_t eof_action = %s_eof_action(tt->current->scope_id);\n", prefix);
  hdwriter_puts(hw, "    if (eof_action <= 0) break;\n");
  hdwriter_printf(hw, "    int64_t err = %s_dispatch_action(ctx, tt, eof_action, len, 0);\n", prefix);
  hdwriter_puts(hw, "    if (err >= 0) return err;\n  }\n  if (tt->current && tt->current->parent_id != -1) return len;\n  return -1;\n}\n\n");

  // final generated C API is emitted by nest.c after PEG and VPA generators.
}

// --- Main entry point ---

void vpa_gen(VpaGenInput* input, HeaderWriter* hw, IrWriter* w, const char* prefix) {
  int32_t n_scopes = (int32_t)darray_size(input->scopes);
  Actions actions = darray_new(sizeof(Action), 0);
  Action sentinel = {.action_id = 0, .action_units = NULL, .begin_scope_id = -1};
  darray_push(actions, sentinel);
  for (int32_t i = 0; i < n_scopes; i++) {
    _gen_scope_dfa(input, w, &input->scopes[i], &actions);
  }
  _gen_init_lexers(input, w, prefix);
  _gen_header(input, hw, prefix, actions);
  darray_del(actions);
}
