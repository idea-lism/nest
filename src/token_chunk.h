#pragma once

#include <stdint.h>

typedef struct {
  int32_t id;
  int32_t start;
  int32_t end;
  int32_t line;
  int32_t col;
} Token;

// TokenChunk is a darray of Token.
typedef Token* TokenChunk;

void tc_init(TokenChunk* c);
void tc_add(TokenChunk* c, Token t);
int32_t tc_size(TokenChunk c);
void tc_free(TokenChunk* c);
