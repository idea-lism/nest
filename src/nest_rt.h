#pragma once

#include "ustr.h"

// read-only functions of darray / token_tree

size_t darray_size(void* a);

// 16 bytes a token
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
Location tt_locate(TokenTree* tree, int32_t cp_offset);

#include "parse_result.inc"
