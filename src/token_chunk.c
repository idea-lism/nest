#include "token_chunk.h"
#include "darray.h"
#include "ustr.h"
#include <stdlib.h>
#include <string.h>

TokenTree* tc_tree_new(const char* ustr) {
  TokenTree* tree = malloc(sizeof(TokenTree));
  tree->src = ustr;
  
  int32_t cp_count = ustr_size(ustr);
  size_t map_size = (cp_count + 63) / 64;
  tree->newline_map = calloc(map_size, sizeof(uint64_t));
  
  tree->table = darray_new(sizeof(TokenChunk), 0);
  
  TokenChunk root = {.scope_id = 0, .parent_id = -1, .tokens = darray_new(sizeof(Token), 0)};
  darray_push(tree->table, root);
  tree->root = &tree->table[0];
  tree->current = tree->root;
  
  return tree;
}

void tc_tree_del(TokenTree* tree) {
  size_t chunk_count = darray_size(tree->table);
  for (size_t i = 0; i < chunk_count; i++) {
    darray_del(tree->table[i].tokens);
  }
  darray_del(tree->table);
  free(tree->newline_map);
  free(tree);
}

Location tc_locate(TokenTree* tree, int32_t cp_offset) {
  int32_t line = 0;
  int32_t col = cp_offset;
  
  int32_t full_words = cp_offset / 64;
  for (int32_t i = 0; i < full_words; i++) {
    line += __builtin_popcountll(tree->newline_map[i]);
  }
  
  int32_t remaining = cp_offset % 64;
  uint64_t mask = (1ULL << remaining) - 1;
  line += __builtin_popcountll(tree->newline_map[full_words] & mask);
  
  for (int32_t i = cp_offset - 1; i >= 0; i--) {
    int32_t word_idx = i / 64;
    int32_t bit_idx = i % 64;
    if (tree->newline_map[word_idx] & (1ULL << bit_idx)) {
      col = cp_offset - i - 1;
      break;
    }
  }
  
  return (Location){.line = line, .col = col};
}

void tc_add(TokenChunk* c, Token t) {
  darray_push(c->tokens, t);
}

TokenChunk* tc_push(TokenTree* tree) {
  int32_t parent_idx = tree->current - tree->table;
  TokenChunk new_chunk = {
    .scope_id = 0,
    .parent_id = parent_idx,
    .tokens = darray_new(sizeof(Token), 0)
  };
  darray_push(tree->table, new_chunk);
  tree->current = &tree->table[darray_size(tree->table) - 1];
  return tree->current;
}

TokenChunk* tc_pop(TokenTree* tree) {
  if (tree->current->parent_id == -1) {
    return tree->current;
  }
  tree->current = &tree->table[tree->current->parent_id];
  return tree->current;
}
