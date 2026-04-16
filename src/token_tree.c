#include "token_tree.h"
#include "darray.h"
#include "ustr.h"
#include <stdlib.h>
#include <string.h>

TokenTree* tt_tree_new(const char* ustr) {
  TokenTree* tree = malloc(sizeof(TokenTree));
  tree->src = ustr;

  int32_t cp_count = ustr_size(ustr);
  size_t map_size = (cp_count + 63) / 64;
  tree->newline_map = calloc(map_size > 0 ? map_size : 1, sizeof(uint64_t));
  tree->table = darray_new(sizeof(TokenChunk), 0);

  TokenChunk root = {.scope_id = 0, .parent_id = -1, .tokens = darray_new(sizeof(Token), 0)};
  darray_push(tree->table, root);
  tree->root = &tree->table[0];
  tree->current = tree->root;

  return tree;
}

void tt_tree_del(TokenTree* tree, bool free_values) {
  if (!tree) {
    return;
  }
  size_t chunk_count = darray_size(tree->table);
  for (size_t i = 0; i < chunk_count; i++) {
    if (free_values && tree->table[i].value) {
      free(tree->table[i].value);
    }
    darray_del(tree->table[i].tokens);
  }
  darray_del(tree->table);
  free(tree->newline_map);
  free(tree);
}

Location tt_locate(TokenTree* tree, int32_t cp_offset) {
  int32_t line = 1;
  int32_t col = cp_offset + 1;

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
      col = cp_offset - i;
      break;
    }
  }

  return (Location){.line = line, .col = col};
}

void tt_mark_newline(TokenTree* tree, int32_t cp_offset) {
  tree->newline_map[cp_offset / 64] |= (1ULL << (cp_offset % 64));
}

void tt_add(TokenTree* tree, int32_t tok_id, int32_t cp_start, int32_t cp_size, int32_t chunk_id) {
  darray_push(tree->current->tokens, ((Token){tok_id, cp_start, cp_size, chunk_id}));
}

TokenChunk* tt_push(TokenTree* tree, int32_t scope_id) {
  int32_t parent_idx = tree->current - tree->table;
  TokenChunk new_chunk = {.scope_id = scope_id, .parent_id = parent_idx, .tokens = darray_new(sizeof(Token), 0)};
  darray_push(tree->table, new_chunk);
  tree->root = &tree->table[0];
  tree->current = &tree->table[darray_size(tree->table) - 1];
  return tree->current;
}

TokenChunk* tt_push_assoc(TokenTree* tree, int32_t scope_id) {
  TokenChunk* chunk = tt_push(tree, scope_id);
  chunk->aux_value = tree;
  return chunk;
}

TokenChunk* tt_pop(TokenTree* tree, int32_t cp_end) {
  if (tree->current->parent_id == -1) {
    fprintf(stderr, "Error: Attempting to pop root chunk, which is not allowed.\n");
    abort();
  }
  TokenChunk* child = tree->current;
  int32_t child_idx = (int32_t)(child - tree->table);
  int32_t scope_id = child->scope_id;
  int32_t scope_cp_start = darray_size(child->tokens) > 0 ? child->tokens[0].cp_start : cp_end;
  int32_t scope_cp_size = cp_end - scope_cp_start;
  tree->current = &tree->table[tree->current->parent_id];
  tt_add(tree, scope_id, scope_cp_start, scope_cp_size, child_idx);
  return tree->current;
}

int32_t tt_current_size(TokenTree* tree) { return darray_size(tree->current->tokens); }

TokenChunk* tt_current(TokenTree* tree) { return tree->current; }

void* tt_alloc_memoize_table(TokenChunk* chunk, int64_t sizeof_col) {
  // alloc sizeof_col * col_token_size
  size_t bytesize = darray_size(chunk->tokens) * sizeof_col;
  void* v = malloc(bytesize);
  memset(v, -1, bytesize);
  chunk->value = v;
  return v;
}
