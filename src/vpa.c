// VPA (Visibly Pushdown Automata) code generation.
// Generates DFA lexer functions in LLVM IR for each scope,
// and emits token ID definitions to the C header.
// Receives pre-analyzed regexp ASTs from parse.c.

#include "vpa.h"
#include "aut.h"
#include "darray.h"
#include "re.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Collect all scope names and their rules, then generate one DFA per scope.

typedef struct {
  char* name; // owned
  VpaRule** rules; // darray
} ScopeInfo;

static void _add_scope_rule(ScopeInfo* scope, VpaRule* rule) {
  if (!scope->rules) {
    scope->rules = darray_new(sizeof(VpaRule*), 0);
  }
  darray_push(scope->rules, rule);
}

// Collect unique token names for header generation
typedef struct {
  char** names; // darray of strdup'd strings
  int32_t* ids; // darray
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

// Walk ReAstNode and build NFA via re.h API

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

  ScopeInfo main_s = {.name = strdup("main"), .rules = NULL};
  darray_push(scopes, main_s);

  for (int32_t i = 0; i < (int32_t)darray_size(rules); i++) {
    if (rules[i].is_macro) {
      continue;
    }
    if (rules[i].is_scope) {
      ScopeInfo s = {.name = strdup(rules[i].name), .rules = NULL};
      darray_push(scopes, s);
      _add_scope_rule(&scopes[darray_size(scopes) - 1], &rules[i]);
    } else {
      _add_scope_rule(&scopes[0], &rules[i]);
    }
  }

  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    if (darray_size(scopes[i].rules) > 0) {
      _gen_scope_dfa(&scopes[i], &reg, w);
    }
  }

  _gen_token_header(&reg, hw);

  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    free(scopes[i].name);
    darray_del(scopes[i].rules);
  }
  darray_del(scopes);
  _free_token_registry(&reg);
}
