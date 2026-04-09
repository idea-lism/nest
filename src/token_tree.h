#pragma once

#include <stdint.h>

typedef struct Token {
  int32_t tok_id;
  int32_t cp_start;
  int32_t cp_size;
  int32_t chunk_id;
} Token;

typedef Token* Tokens;

typedef struct TokenChunk {
  int32_t scope_id;
  int32_t parent_id; // parent chunk_id, can be used "pop"
  void* value;       // parser associate a value to it
  Tokens tokens;     // darray fat pointer
} TokenChunk;

typedef TokenChunk* TokenChunks;

typedef struct TokenTree {
  const char* src;
  uint64_t* newline_map;
  TokenChunk* root;
  TokenChunk* current;
  TokenChunks table; // darray fat pointer
} TokenTree;

typedef struct {
  int32_t line;
  int32_t col;
} Location;

TokenTree* tt_tree_new(const char* ustr);
void tt_tree_del(TokenTree* tree);
Location tt_locate(TokenTree* tree, int32_t cp_offset);
void tt_add(TokenTree* tree, int32_t tok_id, int32_t cp_start, int32_t cp_size, int32_t chunk_id);
TokenChunk* tt_push(TokenTree* tree, int32_t scope_id);
TokenChunk* tt_pop(TokenTree* tree);
int32_t tt_current_size(TokenTree* tree);

TokenChunk* tt_current(TokenTree* tree);
