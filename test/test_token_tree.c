#include "../src/darray.h"
#include "../src/token_tree.h"
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
  TokenTree* tree = tt_tree_new(s);
  assert(tree != NULL);
  assert(tree->root != NULL);
  assert(tree->current != NULL);
  assert(tree->root == tree->current);
  assert(tree->root->parent_id == -1);
  assert(tree->root->scope_id == 0);
  assert(darray_size(tree->root->tokens) == 0);
  assert(darray_size(tree->table) == 1);
  tt_tree_del(tree, false);
  ustr_del(s);
}

// --- Adding tokens ---

TEST(test_add_single) {
  char* s = ustr_new(3, "abc");
  TokenTree* tree = tt_tree_new(s);
  tt_add(tree, 1, 0, 3, -1);
  assert(darray_size(tree->root->tokens) == 1);
  assert(tree->root->tokens[0].term_id == 1);
  assert(tree->root->tokens[0].cp_start == 0);
  assert(tree->root->tokens[0].cp_size == 3);
  assert(tree->root->tokens[0].chunk_id == -1);
  tt_tree_del(tree, false);
  ustr_del(s);
}

TEST(test_add_multiple) {
  char* s = ustr_new(6, "abcdef");
  TokenTree* tree = tt_tree_new(s);
  for (int i = 0; i < 5; i++) {
    tt_add(tree, i, i, 1, -1);
  }
  assert(darray_size(tree->root->tokens) == 5);
  for (int i = 0; i < 5; i++) {
    assert(tree->root->tokens[i].term_id == i);
    assert(tree->root->tokens[i].cp_start == i);
  }
  tt_tree_del(tree, false);
  ustr_del(s);
}

// --- Push / pop ---

TEST(test_push) {
  char* s = ustr_new(3, "abc");
  TokenTree* tree = tt_tree_new(s);
  TokenChunk* child = tt_push(tree, 0);
  assert(child != NULL);
  assert(tree->current == child);
  assert(tree->current != tree->root);
  assert(child->parent_id == 0);
  assert(darray_size(tree->table) == 2);
  tt_tree_del(tree, false);
  ustr_del(s);
}

TEST(test_push_nested) {
  char* s = ustr_new(3, "abc");
  TokenTree* tree = tt_tree_new(s);
  tt_push(tree, 0);
  assert(tree->current->parent_id == 0);
  tt_push(tree, 0);
  assert(tree->current->parent_id == 1);
  assert(darray_size(tree->table) == 3);
  // grandchild -> child -> root
  int32_t gc_parent = tree->current->parent_id;
  int32_t c_parent = tree->table[gc_parent].parent_id;
  assert(c_parent == 0);
  assert(tree->table[c_parent].parent_id == -1);
  tt_tree_del(tree, false);
  ustr_del(s);
}

TEST(test_pop) {
  char* s = ustr_new(3, "abc");
  TokenTree* tree = tt_tree_new(s);
  tt_push(tree, 0);
  assert(tree->current != tree->root);
  TokenChunk* popped = tt_pop(tree, 0);
  assert(popped == tree->current);
  assert(tree->current == &tree->table[0]);
  assert(tree->current->parent_id == -1);
  assert(darray_size(tree->root->tokens) == 1); // scope-ref added by tt_pop
  assert(tree->root->tokens[0].chunk_id == 1);
  tt_tree_del(tree, false);
  ustr_del(s);
}

// tt_pop at root now aborts — no test needed for the disallowed path

TEST(test_push_pop_sequence) {
  char* s = ustr_new(6, "abcdef");
  TokenTree* tree = tt_tree_new(s);
  tt_add(tree, 10, 0, 1, -1);
  tt_push(tree, 0);
  tt_add(tree, 20, 1, 2, -1);
  tt_pop(tree, 3); // cp_end=3
  tt_add(tree, 11, 3, 1, -1);
  // root: [tok(10), scope-ref, tok(11)]
  assert(darray_size(tree->table[0].tokens) == 3);
  assert(tree->table[0].tokens[0].term_id == 10);
  assert(tree->table[0].tokens[1].chunk_id == 1); // scope-ref
  assert(tree->table[0].tokens[1].cp_start == 1);
  assert(tree->table[0].tokens[1].cp_size == 2);
  assert(tree->table[0].tokens[2].term_id == 11);
  // child has 1 token
  assert(darray_size(tree->table[1].tokens) == 1);
  assert(tree->table[1].tokens[0].term_id == 20);
  tt_tree_del(tree, false);
  ustr_del(s);
}

// --- Location lookup ---

TEST(test_locate_no_newlines) {
  char* s = ustr_new(5, "hello");
  TokenTree* tree = tt_tree_new(s);
  // no newlines, everything is line 1
  Location loc0 = tt_locate(tree, 0);
  assert(loc0.line == 1 && loc0.col == 1);
  Location loc3 = tt_locate(tree, 3);
  assert(loc3.line == 1 && loc3.col == 4);
  tt_tree_del(tree, false);
  ustr_del(s);
}

TEST(test_locate_with_newlines) {
  // "ab\ncd\ne" = 7 codepoints, newlines at cp index 2 and 5
  char* s = ustr_new(7, "ab\ncd\ne");
  TokenTree* tree = tt_tree_new(s);
  // manually set newline bits (lexer would do this during codepoint feeding)
  tree->newline_map[0] |= (1ULL << 2);
  tree->newline_map[0] |= (1ULL << 5);

  // 'a' at cp 0 => line 1, col 1
  Location loc = tt_locate(tree, 0);
  assert(loc.line == 1 && loc.col == 1);

  // 'b' at cp 1 => line 1, col 2
  loc = tt_locate(tree, 1);
  assert(loc.line == 1 && loc.col == 2);

  // '\n' at cp 2 => line 1, col 3
  loc = tt_locate(tree, 2);
  assert(loc.line == 1 && loc.col == 3);

  // 'c' at cp 3 => line 2, col 1
  loc = tt_locate(tree, 3);
  assert(loc.line == 2 && loc.col == 1);

  // 'd' at cp 4 => line 2, col 2
  loc = tt_locate(tree, 4);
  assert(loc.line == 2 && loc.col == 2);

  // '\n' at cp 5 => line 2, col 3
  loc = tt_locate(tree, 5);
  assert(loc.line == 2 && loc.col == 3);

  // 'e' at cp 6 => line 3, col 1
  loc = tt_locate(tree, 6);
  assert(loc.line == 3 && loc.col == 1);

  tt_tree_del(tree, false);
  ustr_del(s);
}

TEST(test_locate_at_start_of_line) {
  // "x\ny" = newline at cp 1
  char* s = ustr_new(3, "x\ny");
  TokenTree* tree = tt_tree_new(s);
  // manually set newline bit (lexer would do this during codepoint feeding)
  tree->newline_map[0] |= (1ULL << 1);

  // 'y' at cp 2 => line 2, col 1
  Location loc = tt_locate(tree, 2);
  assert(loc.line == 2 && loc.col == 1);

  tt_tree_del(tree, false);
  ustr_del(s);
}

// --- Multiple chunks with tokens (isolation) ---

TEST(test_tree_structure) {
  char* s = ustr_new(10, "0123456789");
  TokenTree* tree = tt_tree_new(s);

  // root: add a token
  tt_add(tree, 1, 0, 2, -1);
  tt_push(tree, 0);
  tt_add(tree, 2, 2, 3, -1);
  tt_pop(tree, 5);

  tt_push(tree, 0);
  tt_add(tree, 3, 5, 5, -1);
  tt_pop(tree, 10);

  assert(darray_size(tree->table) == 3);

  // root: [tok(1), scope-ref-1, scope-ref-2]
  assert(darray_size(tree->table[0].tokens) == 3);
  assert(tree->table[0].tokens[0].term_id == 1);
  assert(tree->table[0].tokens[1].chunk_id == 1);
  assert(tree->table[0].tokens[1].cp_start == 2);
  assert(tree->table[0].tokens[1].cp_size == 3);
  assert(tree->table[0].tokens[2].chunk_id == 2);
  assert(tree->table[0].tokens[2].cp_start == 5);
  assert(tree->table[0].tokens[2].cp_size == 5);
  assert(tree->table[0].parent_id == -1);
  assert(tree->table[1].tokens[0].term_id == 2);
  assert(tree->table[1].parent_id == 0);
  assert(tree->table[2].tokens[0].term_id == 3);
  assert(tree->table[2].parent_id == 0);
  tt_tree_del(tree, false);
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
  RUN(test_push_pop_sequence);

  RUN(test_locate_no_newlines);
  RUN(test_locate_with_newlines);
  RUN(test_locate_at_start_of_line);

  RUN(test_tree_structure);

  printf("all ok\n");
  return 0;
}
