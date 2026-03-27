// VPA (Visibly Pushdown Automata) code generation.
// Generates DFA lexer functions in LLVM IR for each scope,
// emits runtime data structures and token ID definitions to the C header,
// and produces a VPA dispatch function for scope-aware lexing.

#include "vpa.h"
#include "aut.h"
#include "darray.h"
#include "re.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char* name;
  VpaRule** rules;
  int32_t scope_id;
} ScopeInfo;

static void _add_scope_rule(ScopeInfo* scope, VpaRule* rule) {
  if (!scope->rules) {
    scope->rules = darray_new(sizeof(VpaRule*), 0);
  }
  darray_push(scope->rules, rule);
}

typedef struct {
  char** names;
  int32_t* ids;
} TokenRegistry;

static int32_t _register_token(TokenRegistry* reg, const char* name) {
  int32_t count = (int32_t)darray_size(reg->names);
  for (int32_t i = 0; i < count; i++) {
    if (strcmp(reg->names[i], name) == 0) {
      return reg->ids[i];
    }
  }
  int32_t id = count + 1;
  char* dup = strdup(name);
  darray_push(reg->names, dup);
  darray_push(reg->ids, id);
  return id;
}

static void _gen_token_header(TokenRegistry* reg, HeaderWriter* hw) {
  hw_blank(hw);
  hw_comment(hw, "Token IDs");
  int32_t count = (int32_t)darray_size(reg->names);
  for (int32_t i = 0; i < count; i++) {
    int32_t dn_len = snprintf(NULL, 0, "TOK_%s", reg->names[i]) + 1;
    char define_name[dn_len];
    snprintf(define_name, (size_t)dn_len, "TOK_%s", reg->names[i]);
    for (char* p = define_name + 4; *p; p++) {
      if (*p >= 'a' && *p <= 'z') {
        *p -= 32;
      } else if (*p == '.') {
        *p = '_';
      }
    }
    hw_define(hw, define_name, reg->ids[i]);
  }
}

static void _free_token_registry(TokenRegistry* reg) {
  int32_t count = (int32_t)darray_size(reg->names);
  for (int32_t i = 0; i < count; i++) {
    free(reg->names[i]);
  }
  darray_del(reg->names);
  darray_del(reg->ids);
}

// --- Runtime data structures for the generated header ---

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

  hw_comment(hw, "TokenChunk — matches a scope");
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
  hw_struct_end(hw);
  hw_raw(hw, " TokenTree;\n\n");
}

static void _gen_runtime_helpers(HeaderWriter* hw) {
  hw_blank(hw);
  hw_comment(hw, "TokenTree helpers");

  hw_raw(hw, "static inline TokenChunk* tt_alloc_chunk(TokenTree* tt, int32_t scope_id) {\n");
  hw_raw(hw, "  ChunkTable* ct = &tt->chunk_table;\n");
  hw_raw(hw, "  if (ct->count >= ct->capacity) {\n");
  hw_raw(hw, "    ct->capacity = ct->capacity ? ct->capacity * 2 : 16;\n");
  hw_raw(hw, "    ct->chunks = (TokenChunk*)realloc(ct->chunks, sizeof(TokenChunk) * ct->capacity);\n");
  hw_raw(hw, "  }\n");
  hw_raw(hw, "  TokenChunk* c = &ct->chunks[ct->count];\n");
  hw_raw(hw, "  c->chunk_id = ct->count++;\n");
  hw_raw(hw, "  c->scope_id = scope_id;\n");
  hw_raw(hw, "  c->count = 0;\n");
  hw_raw(hw, "  c->capacity = 32;\n");
  hw_raw(hw, "  c->tokens = (VpaToken*)malloc(sizeof(VpaToken) * 32);\n");
  hw_raw(hw, "  return c;\n");
  hw_raw(hw, "}\n\n");

  hw_raw(hw, "static inline void tt_add_token(TokenChunk* chunk, int32_t tok_id,\n");
  hw_raw(hw, "                                int32_t cp_start, int32_t cp_size) {\n");
  hw_raw(hw, "  if (chunk->count >= chunk->capacity) {\n");
  hw_raw(hw, "    chunk->capacity *= 2;\n");
  hw_raw(hw, "    chunk->tokens = (VpaToken*)realloc(chunk->tokens, sizeof(VpaToken) * chunk->capacity);\n");
  hw_raw(hw, "  }\n");
  hw_raw(hw, "  chunk->tokens[chunk->count++] = (VpaToken){tok_id, cp_start, cp_size, -1};\n");
  hw_raw(hw, "}\n\n");

  hw_raw(hw, "static inline TokenTree* tt_new(int32_t cp_count) {\n");
  hw_raw(hw, "  TokenTree* tt = (TokenTree*)calloc(1, sizeof(TokenTree));\n");
  hw_raw(hw, "  int32_t map_words = (cp_count + 63) / 64;\n");
  hw_raw(hw, "  tt->newline_map_size = map_words;\n");
  hw_raw(hw, "  tt->token_end_map_size = map_words;\n");
  hw_raw(hw, "  tt->newline_map = (uint64_t*)calloc(map_words, sizeof(uint64_t));\n");
  hw_raw(hw, "  tt->token_end_map = (uint64_t*)calloc(map_words, sizeof(uint64_t));\n");
  hw_raw(hw, "  tt->root = tt_alloc_chunk(tt, 0);\n");
  hw_raw(hw, "  tt->current = tt->root;\n");
  hw_raw(hw, "  return tt;\n");
  hw_raw(hw, "}\n\n");

  hw_raw(hw, "static inline void tt_del(TokenTree* tt) {\n");
  hw_raw(hw, "  if (!tt) return;\n");
  hw_raw(hw, "  for (int32_t i = 0; i < tt->chunk_table.count; i++) {\n");
  hw_raw(hw, "    free(tt->chunk_table.chunks[i].tokens);\n");
  hw_raw(hw, "  }\n");
  hw_raw(hw, "  free(tt->chunk_table.chunks);\n");
  hw_raw(hw, "  free(tt->newline_map);\n");
  hw_raw(hw, "  free(tt->token_end_map);\n");
  hw_raw(hw, "  free(tt);\n");
  hw_raw(hw, "}\n\n");

  hw_raw(hw, "static inline void tt_mark_newline(TokenTree* tt, int32_t cp_off) {\n");
  hw_raw(hw, "  tt->newline_map[cp_off / 64] |= (uint64_t)1 << (cp_off % 64);\n");
  hw_raw(hw, "}\n\n");

  hw_raw(hw, "static inline void tt_mark_token_end(TokenTree* tt, int32_t cp_off) {\n");
  hw_raw(hw, "  tt->token_end_map[cp_off / 64] |= (uint64_t)1 << (cp_off % 64);\n");
  hw_raw(hw, "}\n\n");
}

// --- Scope ID definitions ---

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

// --- DFA function declarations in header ---

static void _gen_lex_declarations(ScopeInfo* scopes, HeaderWriter* hw) {
  hw_blank(hw);
  hw_comment(hw, "DFA lexer function declarations");
  hw_raw(hw, "typedef struct { int64_t state; int64_t action; } LexResult;\n");
  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    hw_fmt(hw, "extern LexResult lex_%s(int64_t state, int64_t cp);\n", scopes[i].name);
  }
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

static void _gen_scope_dfa(ScopeInfo* scope, TokenRegistry* reg, IrWriter* w) {
  int32_t fn_len = snprintf(NULL, 0, "lex_%s", scope->name) + 1;
  char func_name[fn_len];
  snprintf(func_name, (size_t)fn_len, "lex_%s", scope->name);

  Aut* aut = aut_new(func_name, "nest");
  Re* re = re_new(aut);
  re_lparen(re);

  bool first = true;
  for (int32_t i = 0; i < (int32_t)darray_size(scope->rules); i++) {
    VpaRule* rule = scope->rules[i];
    for (int32_t j = 0; j < (int32_t)darray_size(rule->units); j++) {
      VpaUnit* unit = &rule->units[j];
      if (unit->kind == VPA_REGEXP && unit->re_ast) {
        const char* tok_name = (unit->name && unit->name[0]) ? unit->name : rule->name;
        int32_t action_id = _register_token(reg, tok_name);
        if (!first) {
          re_fork(re);
        }
        first = false;
        DebugInfo di = {0, 0};
        _emit_re_ast(re, aut, unit->re_ast, di);
        re_action(re, action_id);
      }
    }
  }

  re_rparen(re);
  aut_optimize(aut);
  aut_gen_dfa(aut, w, false);

  re_del(re);
  aut_del(aut);
}

// --- VPA dispatch: C-based scope routing via function pointers ---

static void _gen_dispatch_header(ScopeInfo* scopes, HeaderWriter* hw) {
  hw_blank(hw);
  hw_comment(hw, "VPA scope dispatch");
  hw_raw(hw, "typedef LexResult (*VpaLexFunc)(int64_t, int64_t);\n");

  hw_raw(hw, "static const VpaLexFunc vpa_lex_funcs[] = {\n");
  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    hw_fmt(hw, "  lex_%s,\n", scopes[i].name);
  }
  hw_raw(hw, "};\n\n");

  hw_fmt(hw, "#define VPA_N_SCOPES %d\n\n", (int32_t)darray_size(scopes));

  hw_raw(hw, "static inline LexResult vpa_dispatch(int32_t scope_id, int64_t state, int64_t cp) {\n");
  hw_raw(hw, "  if (scope_id >= 0 && scope_id < VPA_N_SCOPES) {\n");
  hw_raw(hw, "    return vpa_lex_funcs[scope_id](state, cp);\n");
  hw_raw(hw, "  }\n");
  hw_raw(hw, "  return (LexResult){-1, -2};\n");
  hw_raw(hw, "}\n");
}

// --- Public API ---

void vpa_gen(VpaGenInput* input, HeaderWriter* hw, IrWriter* w) {
  VpaRule* rules = input->rules;
  KeywordEntry* keywords = input->keywords;

  TokenRegistry reg = {0};
  reg.names = darray_new(sizeof(char*), 0);
  reg.ids = darray_new(sizeof(int32_t), 0);

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

  ScopeInfo* scopes = darray_new(sizeof(ScopeInfo), 0);

  ScopeInfo main_s = {.name = strdup("main"), .rules = NULL, .scope_id = 0};
  darray_push(scopes, main_s);

  int32_t next_scope_id = 1;
  for (int32_t i = 0; i < (int32_t)darray_size(rules); i++) {
    if (rules[i].is_macro) {
      continue;
    }
    if (rules[i].is_scope) {
      ScopeInfo s = {.name = strdup(rules[i].name), .rules = NULL, .scope_id = next_scope_id++};
      darray_push(scopes, s);
      _add_scope_rule(&scopes[darray_size(scopes) - 1], &rules[i]);
    } else {
      _add_scope_rule(&scopes[0], &rules[i]);
    }
  }

  _gen_runtime_types(hw);
  _gen_runtime_helpers(hw);
  _gen_scope_ids(scopes, hw);
  _gen_lex_declarations(scopes, hw);

  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    if (darray_size(scopes[i].rules) > 0) {
      _gen_scope_dfa(&scopes[i], &reg, w);
    }
  }

  _gen_dispatch_header(scopes, hw);

  _gen_token_header(&reg, hw);

  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    free(scopes[i].name);
    darray_del(scopes[i].rules);
  }
  darray_del(scopes);
  _free_token_registry(&reg);
}
