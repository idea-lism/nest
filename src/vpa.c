// VPA (Visibly Pushdown Automata) code generation.
//
// Generates per-scope DFA lexer functions in LLVM IR,
// an action dispatch function (switch on action_id -> micro-ops),
// an outer lexing loop with scope stack management,
// and emits runtime data structures and token ID definitions to the C header.

#include "vpa.h"
#include "aut.h"
#include "darray.h"
#include "re.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Action registry ---

typedef enum {
  ACTION_TOKEN,
  ACTION_SCOPE_ENTER,
  ACTION_SCOPE_EXIT,
} ActionKind;

typedef struct {
  ActionKind kind;
  int32_t value; // tok_id for TOKEN, scope_id for SCOPE_ENTER/EXIT
  int32_t hook;
  char* user_hook;
} ActionEntry;

typedef struct {
  ActionEntry* entries; // darray (action_id = index + 1)
  char** tok_names;     // darray, parallel
} ActionRegistry;

static int32_t _register_action(ActionRegistry* reg, ActionKind kind, int32_t value, const char* tok_name, int32_t hook,
                                const char* user_hook) {
  int32_t id = (int32_t)darray_size(reg->entries) + 1;
  ActionEntry e = {.kind = kind, .value = value, .hook = hook, .user_hook = user_hook ? strdup(user_hook) : NULL};
  darray_push(reg->entries, e);
  char* tn = tok_name ? strdup(tok_name) : NULL;
  darray_push(reg->tok_names, tn);
  return id;
}

static int32_t _register_token(ActionRegistry* reg, const char* tok_name) {
  int32_t count = (int32_t)darray_size(reg->entries);
  for (int32_t i = 0; i < count; i++) {
    if (reg->entries[i].kind == ACTION_TOKEN && reg->tok_names[i] && strcmp(reg->tok_names[i], tok_name) == 0) {
      return i + 1;
    }
  }
  int32_t tok_id = count + 1;
  return _register_action(reg, ACTION_TOKEN, tok_id, tok_name, 0, NULL);
}

static void _free_action_registry(ActionRegistry* reg) {
  int32_t count = (int32_t)darray_size(reg->entries);
  for (int32_t i = 0; i < count; i++) {
    free(reg->entries[i].user_hook);
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
  ReAstNode* ast;
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

// --- Resolve scope body into DFA patterns ---

static DfaPattern* _resolve_body(VpaUnit* body, VpaRule* rules, ScopeInfo* scopes, ActionRegistry* reg) {
  DfaPattern* patterns = darray_new(sizeof(DfaPattern), 0);

  for (int32_t i = 0; i < (int32_t)darray_size(body); i++) {
    VpaUnit* u = &body[i];

    if (u->kind == VPA_REGEXP && u->re_ast) {
      const char* tok_name = (u->name && u->name[0]) ? u->name : NULL;
      int32_t action_id;
      if (u->hook != 0) {
        int32_t tok_id = tok_name ? _register_token(reg, tok_name) : 0;
        action_id = _register_action(reg, ACTION_SCOPE_EXIT, tok_id, tok_name, u->hook, u->user_hook);
      } else if (tok_name) {
        action_id = _register_token(reg, tok_name);
      } else {
        continue;
      }
      darray_push(patterns, ((DfaPattern){.ast = u->re_ast, .action_id = action_id}));

    } else if (u->kind == VPA_REF && u->name) {
      VpaRule* ref_rule = _find_rule(rules, u->name);
      if (!ref_rule) {
        continue;
      }
      VpaUnit* scope_unit = _find_scope_unit(ref_rule);
      if (scope_unit && scope_unit->re_ast) {
        ScopeInfo* target = _find_scope(scopes, ref_rule->name);
        if (target) {
          int32_t aid = _register_action(reg, ACTION_SCOPE_ENTER, target->scope_id, NULL, scope_unit->hook,
                                         scope_unit->user_hook);
          darray_push(patterns, ((DfaPattern){.ast = scope_unit->re_ast, .action_id = aid}));
        }
      }
      for (int32_t j = 0; j < (int32_t)darray_size(ref_rule->units); j++) {
        VpaUnit* ru = &ref_rule->units[j];
        if (ru->kind == VPA_REGEXP && ru->re_ast) {
          const char* tn = (ru->name && ru->name[0]) ? ru->name : ref_rule->name;
          int32_t aid = _register_token(reg, tn);
          darray_push(patterns, ((DfaPattern){.ast = ru->re_ast, .action_id = aid}));
        }
      }

    } else if (u->kind == VPA_SCOPE && u->re_ast) {
      ScopeInfo* target = _find_scope(scopes, u->name ? u->name : "");
      if (target) {
        int32_t aid = _register_action(reg, ACTION_SCOPE_ENTER, target->scope_id, NULL, u->hook, u->user_hook);
        darray_push(patterns, ((DfaPattern){.ast = u->re_ast, .action_id = aid}));
      }
    }
  }

  return patterns;
}

// --- Build DFA from resolved patterns ---

static void _gen_scope_dfa(ScopeInfo* scope, DfaPattern* patterns, IrWriter* w) {
  if ((int32_t)darray_size(patterns) == 0) {
    return;
  }

  int32_t fn_len = snprintf(NULL, 0, "lex_%s", scope->name) + 1;
  char func_name[fn_len];
  snprintf(func_name, (size_t)fn_len, "lex_%s", scope->name);

  Aut* aut = aut_new(func_name, "nest");
  Re* re = re_new(aut);
  re_lparen(re);

  for (int32_t i = 0; i < (int32_t)darray_size(patterns); i++) {
    if (i > 0) {
      re_fork(re);
    }
    DebugInfo di = {0, 0};
    _emit_re_ast(re, aut, patterns[i].ast, di);
    re_action(re, patterns[i].action_id);
  }

  re_rparen(re);
  aut_optimize(aut);
  aut_gen_dfa(aut, w, false);

  re_del(re);
  aut_del(aut);
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

  hw_comment(hw, "Runtime callbacks (implement these to customize behavior)");
  hw_raw(hw, "void vpa_rt_emit_token(void* tt, int32_t tok_id, int32_t cp_start, int32_t cp_size);\n");
  hw_raw(hw, "void vpa_rt_push_scope(void* tt, int32_t scope_id);\n");
  hw_raw(hw, "void vpa_rt_pop_scope(void* tt);\n");
  hw_raw(hw, "int32_t vpa_rt_read_cp(void* src, int32_t cp_off);\n");
  hw_raw(hw, "int32_t vpa_rt_get_scope(void* tt);\n\n");
}

static void _gen_token_header(ActionRegistry* reg, HeaderWriter* hw) {
  hw_blank(hw);
  hw_comment(hw, "Token IDs");
  int32_t count = (int32_t)darray_size(reg->entries);
  for (int32_t i = 0; i < count; i++) {
    if (reg->entries[i].kind != ACTION_TOKEN || !reg->tok_names[i]) {
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
    hw_define(hw, define_name, i + 1);
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

    switch (e->kind) {
    case ACTION_TOKEN:
      irwriter_rawf(w, "  call void @vpa_rt_emit_token(ptr %%tt, i32 %d, i32 %%cp_start, i32 %%cp_size)\n", i + 1);
      break;
    case ACTION_SCOPE_ENTER:
      irwriter_rawf(w, "  call void @vpa_rt_push_scope(ptr %%tt, i32 %d)\n", e->value);
      break;
    case ACTION_SCOPE_EXIT:
      if (e->value > 0) {
        irwriter_rawf(w, "  call void @vpa_rt_emit_token(ptr %%tt, i32 %d, i32 %%cp_start, i32 %%cp_size)\n", e->value);
      }
      irwriter_raw(w, "  call void @vpa_rt_pop_scope(ptr %tt)\n");
      break;
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

  irwriter_raw(w, "  store i32 0, ptr %cp_off\n");
  irwriter_raw(w, "  store i32 0, ptr %state\n");
  irwriter_raw(w, "  store i32 0, ptr %tok_start\n");
  irwriter_raw(w, "  store i32 0, ptr %last_act\n");
  irwriter_raw(w, "  store i32 0, ptr %last_off\n");
  irwriter_raw(w, "  br label %Lloop\n\n");

  // --- Lloop: check if we reached the end ---
  irwriter_raw(w, "Lloop:\n");
  irwriter_raw(w, "  %off.0 = load i32, ptr %cp_off\n");
  irwriter_raw(w, "  %at_end = icmp sge i32 %off.0, %len\n");
  irwriter_raw(w, "  br i1 %at_end, label %Lflush, label %Lfeed\n\n");

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
  irwriter_raw(w, "  %has_accept = icmp sgt i32 %la, 0\n");
  irwriter_raw(w, "  br i1 %has_accept, label %Ldispatch, label %Lskip\n\n");

  // --- Ldispatch: call vpa_dispatch, reset DFA, continue ---
  irwriter_raw(w, "Ldispatch:\n");
  irwriter_raw(w, "  %ts = load i32, ptr %tok_start\n");
  irwriter_raw(w, "  %lo = load i32, ptr %last_off\n");
  irwriter_raw(w, "  %sz = sub i32 %lo, %ts\n");
  irwriter_raw(w, "  call void @vpa_dispatch(ptr %tt, i32 %la, i32 %ts, i32 %sz)\n");
  // Reset: cp_off = last_off, state = 0, tok_start = last_off, clear last_act
  irwriter_raw(w, "  store i32 %lo, ptr %cp_off\n");
  irwriter_raw(w, "  store i32 0, ptr %state\n");
  irwriter_raw(w, "  store i32 %lo, ptr %tok_start\n");
  irwriter_raw(w, "  store i32 0, ptr %last_act\n");
  irwriter_raw(w, "  store i32 0, ptr %last_off\n");
  irwriter_raw(w, "  br label %Lloop\n\n");

  // --- Lskip: no accepting state, skip one codepoint ---
  irwriter_raw(w, "Lskip:\n");
  irwriter_raw(w, "  %off.6 = load i32, ptr %tok_start\n");
  irwriter_raw(w, "  %off.7 = add i32 %off.6, 1\n");
  irwriter_raw(w, "  store i32 %off.7, ptr %cp_off\n");
  irwriter_raw(w, "  store i32 %off.7, ptr %tok_start\n");
  irwriter_raw(w, "  store i32 0, ptr %state\n");
  irwriter_raw(w, "  store i32 0, ptr %last_act\n");
  irwriter_raw(w, "  br label %Lloop\n\n");

  // --- Lflush: end of input, dispatch any pending accept ---
  irwriter_raw(w, "Lflush:\n");
  irwriter_raw(w, "  %la2 = load i32, ptr %last_act\n");
  irwriter_raw(w, "  %has2 = icmp sgt i32 %la2, 0\n");
  irwriter_raw(w, "  br i1 %has2, label %Lflush_dispatch, label %Ldone\n\n");

  irwriter_raw(w, "Lflush_dispatch:\n");
  irwriter_raw(w, "  %ts2 = load i32, ptr %tok_start\n");
  irwriter_raw(w, "  %lo2 = load i32, ptr %last_off\n");
  irwriter_raw(w, "  %sz2 = sub i32 %lo2, %ts2\n");
  irwriter_raw(w, "  call void @vpa_dispatch(ptr %tt, i32 %la2, i32 %ts2, i32 %sz2)\n");
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

  ActionRegistry reg = {0};
  reg.entries = darray_new(sizeof(ActionEntry), 0);
  reg.tok_names = darray_new(sizeof(char*), 0);

  // Pre-register keywords as tokens
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

  // Collect scopes
  ScopeInfo* scopes = _collect_scopes(rules);

  // Emit header: types + helpers
  _gen_runtime_types(hw);
  _gen_runtime_helpers(hw);
  _gen_scope_ids(scopes, hw);

  // Resolve and build DFA per scope
  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    if (!scopes[i].body) {
      continue;
    }
    DfaPattern* patterns = _resolve_body(scopes[i].body, rules, scopes, &reg);
    _gen_scope_dfa(&scopes[i], patterns, w);
    darray_del(patterns);
  }

  // Emit action dispatch and lex loop in IR
  _gen_action_dispatch_ir(&reg, w);
  _gen_lex_loop_ir(scopes, w);

  // Emit header: declarations, token IDs, action metadata
  _gen_lex_declarations(scopes, hw);
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
