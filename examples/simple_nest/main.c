#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VPA_IMPLEMENTATION
#include "grammar.h"

int32_t vpa_rt_read_cp(void* src, int32_t cp_off) { return ((const unsigned char*)src)[cp_off]; }

static const char* tok_name(int32_t id) {
  switch (id) {
  case TOK_KW_IF:
    return "kw_if";
  case TOK_KW_ELSE:
    return "kw_else";
  case TOK_KW_WHILE:
    return "kw_while";
  case TOK_IDENT:
    return "ident";
  case TOK_NUMBER:
    return "number";
  case TOK_SPACE:
    return "space";
  case TOK_PLUS:
    return "+";
  case TOK_MINUS:
    return "-";
  case TOK_STAR:
    return "*";
  case TOK_SLASH:
    return "/";
  case TOK_ASSIGN:
    return "=";
  case TOK_SEMI:
    return ";";
  case TOK_LPAREN:
    return "(";
  case TOK_RPAREN:
    return ")";
  case TOK_LBRACE:
    return "{";
  case TOK_RBRACE:
    return "}";
  case TOK_EQ:
    return "==";
  case TOK_NE:
    return "!=";
  case TOK_LT:
    return "<";
  case TOK_GT:
    return ">";
  case TOK_LE:
    return "<=";
  case TOK_GE:
    return ">=";
  default:
    return "?";
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <input>\n", argv[0]);
    return 1;
  }

  const char* input = argv[1];
  int32_t len = (int32_t)strlen(input);

  TokenTree* tt = tt_new(len);
  vpa_lex((int64_t)(intptr_t)input, (int64_t)len, (int64_t)(intptr_t)tt);

  printf("%d chunk(s), %d token(s) in root\n", tt->chunk_table.count, tt->root->count);
  for (int32_t c = 0; c < tt->chunk_table.count; c++) {
    TokenChunk* chunk = &tt->chunk_table.chunks[c];
    if (c > 0) {
      printf("--- scope %d ---\n", chunk->scope_id);
    }
    for (int32_t i = 0; i < chunk->count; i++) {
      VpaToken* tok = &chunk->tokens[i];
      printf("  %-10s (id=%2d) \"%.*s\"\n", tok_name(tok->tok_id), tok->tok_id, tok->cp_size, input + tok->cp_start);
    }
  }

  tt_del(tt);
  return 0;
}
