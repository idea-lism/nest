#pragma once

#include <stdint.h>

// 16 bytes a token
typedef struct {
  int32_t term_id; // token_id or scope_id (in the same numbering system), parse analysis should give a universal
                   // numbering to all of them
  // with cp_start (absolute offset relative to input string):
  // - we can locate the line & column with newline_map
  // - we can locate the byte offset by ustr
  int32_t cp_start;
  int32_t cp_size;
  int32_t chunk_id; // when term_id is a scope, it can be expanded to a TokenChunk
} Token;

typedef Token* Tokens;

typedef struct TokenChunk {
  int32_t scope_id;
  int32_t parent_id; // parent chunk_id, -1 for root chunk, can be used by "pop"
  void* value;       // parser associate a value to it after parse, `struct ScopeXxx`
  void* aux_value;   // parser associate another value to it
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
void tt_tree_del(TokenTree* tree, bool free_values);
Location tt_locate(TokenTree* tree, int32_t cp_offset);
void tt_add(TokenTree* tree, int32_t tok_id, int32_t cp_start, int32_t cp_size, int32_t chunk_id);
TokenChunk* tt_push(TokenTree* tree, int32_t scope_id);
TokenChunk* tt_pop(TokenTree* tree);

// current chunk token count
int32_t tt_current_size(TokenTree* tree);
// get current chunk
TokenChunk* tt_current(TokenTree* tree);
// alloc sizeof_col * col_token_size to value
void* tt_alloc_memoize_table(TokenChunk* chunk, int64_t sizeof_col);
