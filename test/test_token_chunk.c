#include "../src/darray.h"
#include "../src/token_chunk.h"
#include "../src/ustr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

// --- Tree creation / deletion ---

TEST(test_tree_new) {
  char* s = ustr_new(5, "hello");
  TokenTree* tree = tc_tree_new(s);
  assert(tree != NULL);
  assert(tree->root != NULL);
  assert(tree->current != NULL);
  assert(tree->root == tree->current);
  assert(tree->root->parent_id == -1);
  assert(tree->root->scope_id == 0);
  assert(darray_size(tree->root->tokens) == 0);
  assert(darray_size(tree->table) == 1);
  tc_tree_del(tree);
  ustr_del(s);
}

// --- Adding tokens ---

TEST(test_add_single) {
  char* s = ustr_new(3, "abc");
  TokenTree* tree = tc_tree_new(s);
  Token t = {.tok_id = 1, .cp_start = 0, .cp_size = 3, .chunk_id = -1};
  tc_add(tree->root, t);
  assert(darray_size(tree->root->tokens) == 1);
  assert(tree->root->tokens[0].tok_id == 1);
  assert(tree->root->tokens[0].cp_start == 0);
  assert(tree->root->tokens[0].cp_size == 3);
  assert(tree->root->tokens[0].chunk_id == -1);
  tc_tree_del(tree);
  ustr_del(s);
}

TEST(test_add_multiple) {
  char* s = ustr_new(6, "abcdef");
  TokenTree* tree = tc_tree_new(s);
  for (int i = 0; i < 5; i++) {
    Token t = {.tok_id = i, .cp_start = i, .cp_size = 1, .chunk_id = -1};
    tc_add(tree->root, t);
  }
  assert(darray_size(tree->root->tokens) == 5);
  for (int i = 0; i < 5; i++) {
    assert(tree->root->tokens[i].tok_id == i);
    assert(tree->root->tokens[i].cp_start == i);
  }
  tc_tree_del(tree);
  ustr_del(s);
}

// --- Push / pop ---

TEST(test_push) {
  char* s = ustr_new(3, "abc");
  TokenTree* tree = tc_tree_new(s);
  TokenChunk* child = tc_push(tree);
  assert(child != NULL);
  assert(tree->current == child);
  assert(tree->current != tree->root);
  assert(child->parent_id == 0);
  assert(darray_size(tree->table) == 2);
  tc_tree_del(tree);
  ustr_del(s);
}

TEST(test_push_nested) {
  char* s = ustr_new(3, "abc");
  TokenTree* tree = tc_tree_new(s);
  tc_push(tree);
  assert(tree->current->parent_id == 0);
  tc_push(tree);
  assert(tree->current->parent_id == 1);
  assert(darray_size(tree->table) == 3);
  // grandchild -> child -> root
  int32_t gc_parent = tree->current->parent_id;
  int32_t c_parent = tree->table[gc_parent].parent_id;
  assert(c_parent == 0);
  assert(tree->table[c_parent].parent_id == -1);
  tc_tree_del(tree);
  ustr_del(s);
}

TEST(test_pop) {
  char* s = ustr_new(3, "abc");
  TokenTree* tree = tc_tree_new(s);
  tc_push(tree);
  assert(tree->current != tree->root);
  TokenChunk* popped = tc_pop(tree);
  assert(popped == tree->current);
  assert(tree->current == &tree->table[0]);
  assert(tree->current->parent_id == -1);
  tc_tree_del(tree);
  ustr_del(s);
}

TEST(test_pop_at_root) {
  char* s = ustr_new(3, "abc");
  TokenTree* tree = tc_tree_new(s);
  TokenChunk* before = tree->current;
  TokenChunk* after = tc_pop(tree);
  assert(after == before);
  assert(tree->current->parent_id == -1);
  tc_tree_del(tree);
  ustr_del(s);
}

TEST(test_push_pop_sequence) {
  char* s = ustr_new(6, "abcdef");
  TokenTree* tree = tc_tree_new(s);

  // add token to root
  tc_add(tree->current, (Token){.tok_id = 10, .cp_start = 0, .cp_size = 1, .chunk_id = -1});

  // push child, add token there
  tc_push(tree);
  tc_add(tree->current, (Token){.tok_id = 20, .cp_start = 1, .cp_size = 2, .chunk_id = -1});

  // pop back to root, add another token
  tc_pop(tree);
  tc_add(tree->current, (Token){.tok_id = 11, .cp_start = 3, .cp_size = 1, .chunk_id = -1});

  // root has 2 tokens, child has 1
  assert(darray_size(tree->table[0].tokens) == 2);
  assert(tree->table[0].tokens[0].tok_id == 10);
  assert(tree->table[0].tokens[1].tok_id == 11);
  assert(darray_size(tree->table[1].tokens) == 1);
  assert(tree->table[1].tokens[0].tok_id == 20);

  tc_tree_del(tree);
  ustr_del(s);
}

// --- Location lookup ---

TEST(test_locate_no_newlines) {
  char* s = ustr_new(5, "hello");
  TokenTree* tree = tc_tree_new(s);
  // no newlines set in map, everything is line 0
  Location loc0 = tc_locate(tree, 0);
  assert(loc0.line == 0 && loc0.col == 0);
  Location loc3 = tc_locate(tree, 3);
  assert(loc3.line == 0 && loc3.col == 3);
  tc_tree_del(tree);
  ustr_del(s);
}

TEST(test_locate_with_newlines) {
  // "ab\ncd\ne" = 7 codepoints, newlines at cp index 2 and 5
  char* s = ustr_new(7, "ab\ncd\ne");
  TokenTree* tree = tc_tree_new(s);
  // set newline bits at positions 2 and 5
  tree->newline_map[0] |= (1ULL << 2);
  tree->newline_map[0] |= (1ULL << 5);

  // 'a' at cp 0 => line 0, col 0
  Location loc = tc_locate(tree, 0);
  assert(loc.line == 0 && loc.col == 0);

  // 'b' at cp 1 => line 0, col 1
  loc = tc_locate(tree, 1);
  assert(loc.line == 0 && loc.col == 1);

  // '\n' at cp 2 => line 0, col 2
  loc = tc_locate(tree, 2);
  assert(loc.line == 0 && loc.col == 2);

  // 'c' at cp 3 => line 1, col 0
  loc = tc_locate(tree, 3);
  assert(loc.line == 1 && loc.col == 0);

  // 'd' at cp 4 => line 1, col 1
  loc = tc_locate(tree, 4);
  assert(loc.line == 1 && loc.col == 1);

  // '\n' at cp 5 => line 1, col 2
  loc = tc_locate(tree, 5);
  assert(loc.line == 1 && loc.col == 2);

  // 'e' at cp 6 => line 2, col 0
  loc = tc_locate(tree, 6);
  assert(loc.line == 2 && loc.col == 0);

  tc_tree_del(tree);
  ustr_del(s);
}

TEST(test_locate_at_start_of_line) {
  // "x\ny" = newline at cp 1
  char* s = ustr_new(3, "x\ny");
  TokenTree* tree = tc_tree_new(s);
  tree->newline_map[0] |= (1ULL << 1);

  // 'y' at cp 2 => line 1, col 0
  Location loc = tc_locate(tree, 2);
  assert(loc.line == 1 && loc.col == 0);

  tc_tree_del(tree);
  ustr_del(s);
}

// --- Multiple chunks with tokens (isolation) ---

TEST(test_tree_structure) {
  char* s = ustr_new(10, "0123456789");
  TokenTree* tree = tc_tree_new(s);

  // root: add a token
  tc_add(tree->current, (Token){.tok_id = 1, .cp_start = 0, .cp_size = 2, .chunk_id = -1});

  // push child1, add token, pop
  tc_push(tree);
  tc_add(tree->current, (Token){.tok_id = 2, .cp_start = 2, .cp_size = 3, .chunk_id = -1});
  tc_pop(tree);

  // push child2, add token, pop
  tc_push(tree);
  tc_add(tree->current, (Token){.tok_id = 3, .cp_start = 5, .cp_size = 5, .chunk_id = -1});
  tc_pop(tree);

  assert(darray_size(tree->table) == 3);

  // root chunk (index 0)
  assert(darray_size(tree->table[0].tokens) == 1);
  assert(tree->table[0].tokens[0].tok_id == 1);
  assert(tree->table[0].parent_id == -1);

  // child1 (index 1)
  assert(darray_size(tree->table[1].tokens) == 1);
  assert(tree->table[1].tokens[0].tok_id == 2);
  assert(tree->table[1].parent_id == 0);

  // child2 (index 2)
  assert(darray_size(tree->table[2].tokens) == 1);
  assert(tree->table[2].tokens[0].tok_id == 3);
  assert(tree->table[2].parent_id == 0);

  tc_tree_del(tree);
  ustr_del(s);
}

int main(void) {
  printf("test_token_chunk:\n");

  RUN(test_tree_new);

  RUN(test_add_single);
  RUN(test_add_multiple);

  RUN(test_push);
  RUN(test_push_nested);
  RUN(test_pop);
  RUN(test_pop_at_root);
  RUN(test_push_pop_sequence);

  RUN(test_locate_no_newlines);
  RUN(test_locate_with_newlines);
  RUN(test_locate_at_start_of_line);

  RUN(test_tree_structure);

  printf("all ok\n");
  return 0;
}
