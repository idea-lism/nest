#pragma once

#include <stdint.h>

typedef struct __attribute__((packed, aligned(8))) {
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

typedef struct __attribute__((packed, aligned(8))) TokenChunk {
  int32_t scope_id;
  int32_t parent_id; // parent chunk_id, -1 for root chunk, can be used by "pop"
  void* value;       // parser associate a value to it after parse, `struct ScopeXxx`
  void* aux_value;   // parser associate another value to it
  Tokens tokens;     // darray fat pointer
} TokenChunk;

typedef TokenChunk* TokenChunks;

typedef struct __attribute__((packed, aligned(8))) TokenTree {
  const char* src;
  uint64_t* newline_map;
  TokenChunk* root;
  TokenChunk* current;
  TokenChunks table; // darray fat pointer
} TokenTree;

// 1-based line and column from tt_locate
typedef struct __attribute__((packed, aligned(8))) {
  int32_t line; // 1-based
  int32_t col;  // 1-based
} Location;

TokenTree* tt_tree_new(const char* ustr);
void tt_tree_del(TokenTree* tree, bool free_values);
Location tt_locate(TokenTree* tree, int32_t cp_offset);
// mark codepoint at cp_offset as newline in the bitmap (called by lexer for each '\n')
void tt_mark_newline(TokenTree* tree, int32_t cp_offset);
void tt_add(TokenTree* tree, int32_t tok_id, int32_t cp_start, int32_t cp_size, int32_t chunk_id);
TokenChunk* tt_push(TokenTree* tree, int32_t scope_id);
// generated parser only: also set the token_chunk.aux_value = token_tree
TokenChunk* tt_push_assoc(TokenTree* tree, int32_t scope_id);
// Pop current chunk. Add scope-ref token to parent chunk.
// cp_end = cp_start + cp_size of the closing token.
// scope-ref: term_id = child scope_id, chunk_id = child index in table,
//   cp_start = child's first token's cp_start, cp_size = cp_end - cp_start.
TokenChunk* tt_pop(TokenTree* tree, int32_t cp_end);

// current chunk token count
int64_t tt_current_size(TokenTree* tree);
// get current chunk
TokenChunk* tt_current(TokenTree* tree);
// alloc sizeof_col * (token_count + 1) to value, +1 for sentinel column
// sizeof_col must be multiple of 8, returned address must be 64-bit aligned
void* tt_alloc_memoize_table(TokenChunk* chunk, int64_t sizeof_col);
