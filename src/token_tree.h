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

TokenTree* tc_tree_new(const char* ustr);
void tc_tree_del(TokenTree* tree);
Location tc_locate(TokenTree* tree, int32_t cp_offset);
void tc_add(TokenChunk* c, Token t);
TokenChunk* tc_push(TokenTree* tree);
TokenChunk* tc_pop(TokenTree* tree);
