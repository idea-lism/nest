#pragma once

#include <stdint.h>

typedef struct {
  int32_t tok_id;
  int32_t cp_start;
  int32_t cp_size;
  int32_t chunk_id;
} Token;

typedef struct {
  int32_t scope_id;
  int32_t parent_id;
  Token* tokens;
} TokenChunk;

void tc_init(TokenChunk* c, int32_t scope_id, int32_t parent_id);
void tc_add(TokenChunk* c, Token t);
int32_t tc_size(TokenChunk* c);
void tc_free(TokenChunk* c);
