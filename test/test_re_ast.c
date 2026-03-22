#include "../src/re_ast.h"
#include "../src/parse.h"
#include "../src/darray.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

// --- re_ast_free ---

TEST(test_free_null) {
  re_ast_free(NULL); // must not crash
}

TEST(test_free_leaf) {
  ReAstNode node = {.kind = RE_AST_CHAR, .codepoint = 'x'};
  re_ast_free(&node); // no children, no-op
}

TEST(test_free_with_children) {
  ReAstNode* ast = calloc(1, sizeof(ReAstNode));
  ast->kind = RE_AST_SEQ;
  re_ast_add_child(ast, (ReAstNode){.kind = RE_AST_CHAR, .codepoint = 'a'});
  re_ast_add_child(ast, (ReAstNode){.kind = RE_AST_CHAR, .codepoint = 'b'});
  re_ast_free(ast);
  free(ast);
}

// --- re_ast_clone ---

TEST(test_clone_null) {
  assert(re_ast_clone(NULL) == NULL);
}

TEST(test_clone_leaf) {
  ReAstNode src = {.kind = RE_AST_CHAR, .codepoint = 42};
  ReAstNode* dst = re_ast_clone(&src);
  assert(dst->kind == RE_AST_CHAR);
  assert(dst->codepoint == 42);
  assert(dst->children == NULL);
  re_ast_free(dst);
  free(dst);
}

TEST(test_clone_deep) {
  ReAstNode src = {.kind = RE_AST_SEQ};
  re_ast_add_child(&src, (ReAstNode){.kind = RE_AST_CHAR, .codepoint = 'x'});
  re_ast_add_child(&src, (ReAstNode){.kind = RE_AST_DOT});

  ReAstNode* dst = re_ast_clone(&src);
  assert(dst->kind == RE_AST_SEQ);
  assert((int32_t)darray_size(dst->children) == 2);
  assert(dst->children[0].kind == RE_AST_CHAR);
  assert(dst->children[0].codepoint == 'x');
  assert(dst->children[1].kind == RE_AST_DOT);
  // must be independent copies
  assert(dst->children != src.children);

  re_ast_free(dst);
  free(dst);
  re_ast_free(&src);
}

// --- re_ast_add_child ---

TEST(test_add_child) {
  ReAstNode parent = {.kind = RE_AST_SEQ};
  re_ast_add_child(&parent, (ReAstNode){.kind = RE_AST_CHAR, .codepoint = 'a'});
  re_ast_add_child(&parent, (ReAstNode){.kind = RE_AST_CHAR, .codepoint = 'b'});
  re_ast_add_child(&parent, (ReAstNode){.kind = RE_AST_CHAR, .codepoint = 'c'});
  assert((int32_t)darray_size(parent.children) == 3);
  assert(parent.children[2].codepoint == 'c');
  re_ast_free(&parent);
}

// --- re_ast_build_literal ---

TEST(test_build_literal_ascii) {
  const char* src = "hello";
  ReAstNode* ast = re_ast_build_literal(src, 0, 5);
  assert(ast->kind == RE_AST_SEQ);
  assert((int32_t)darray_size(ast->children) == 5);
  assert(ast->children[0].kind == RE_AST_CHAR);
  assert(ast->children[0].codepoint == 'h');
  assert(ast->children[4].codepoint == 'o');
  re_ast_free(ast);
  free(ast);
}

TEST(test_build_literal_offset) {
  const char* src = "xxABCxx";
  ReAstNode* ast = re_ast_build_literal(src, 2, 3);
  assert((int32_t)darray_size(ast->children) == 3);
  assert(ast->children[0].codepoint == 'A');
  assert(ast->children[2].codepoint == 'C');
  re_ast_free(ast);
  free(ast);
}

TEST(test_build_literal_utf8) {
  // U+00E9 (é) = 0xC3 0xA9
  const char* src = "\xC3\xA9";
  ReAstNode* ast = re_ast_build_literal(src, 0, 2);
  assert((int32_t)darray_size(ast->children) == 1);
  assert(ast->children[0].codepoint == 0xE9);
  re_ast_free(ast);
  free(ast);
}

// --- re_ast_parse_char_cp ---

TEST(test_parse_char_plain) {
  const char* src = "a";
  ReToken t = {TOK_CHAR, 0, 1};
  assert(re_ast_parse_char_cp(src, &t) == 'a');
}

TEST(test_parse_char_c_escape_n) {
  const char* src = "\\n";
  ReToken t = {TOK_C_ESCAPE, 0, 2};
  assert(re_ast_parse_char_cp(src, &t) == '\n');
}

TEST(test_parse_char_c_escape_t) {
  const char* src = "\\t";
  ReToken t = {TOK_C_ESCAPE, 0, 2};
  assert(re_ast_parse_char_cp(src, &t) == '\t');
}

TEST(test_parse_char_codepoint) {
  const char* src = "\\u{41}";
  ReToken t = {TOK_CODEPOINT, 0, 6};
  assert(re_ast_parse_char_cp(src, &t) == 0x41);
}

TEST(test_parse_char_codepoint_4digit) {
  const char* src = "\\u{1F600}";
  ReToken t = {TOK_CODEPOINT, 0, 9};
  assert(re_ast_parse_char_cp(src, &t) == 0x1F600);
}

TEST(test_parse_char_plain_escape) {
  const char* src = "\\.";
  ReToken t = {TOK_PLAIN_ESCAPE, 0, 2};
  assert(re_ast_parse_char_cp(src, &t) == '.');
}

// --- re_ast_build_charclass ---

TEST(test_charclass_single) {
  const char* src = "a";
  ReToken tokens[] = {{TOK_CHAR, 0, 1}};
  ReAstNode node = re_ast_build_charclass(src, tokens, 1, false);
  assert(node.kind == RE_AST_CHARCLASS);
  assert(!node.negated);
  assert((int32_t)darray_size(node.children) == 1);
  assert(node.children[0].kind == RE_AST_CHAR);
  assert(node.children[0].codepoint == 'a');
  re_ast_free(&node);
}

TEST(test_charclass_negated) {
  const char* src = "x";
  ReToken tokens[] = {{TOK_CHAR, 0, 1}};
  ReAstNode node = re_ast_build_charclass(src, tokens, 1, true);
  assert(node.negated);
  re_ast_free(&node);
}

TEST(test_charclass_range) {
  // "a-z" as tokens: RANGE_START('a'), CHAR('z')
  const char* src = "az";
  ReToken tokens[] = {{TOK_RANGE_START, 0, 1}, {TOK_CHAR, 1, 2}};
  ReAstNode node = re_ast_build_charclass(src, tokens, 2, false);
  assert((int32_t)darray_size(node.children) == 1);
  assert(node.children[0].kind == RE_AST_RANGE);
  assert(node.children[0].range_lo == 'a');
  assert(node.children[0].range_hi == 'z');
  re_ast_free(&node);
}

// --- re_ast_build_re ---

TEST(test_re_single_char) {
  const char* src = "x";
  ReToken tokens[] = {{TOK_CHAR, 0, 1}};
  ReAstNode* ast = re_ast_build_re(src, tokens, 1, NULL, 0);
  assert(ast->kind == RE_AST_CHAR);
  assert(ast->codepoint == 'x');
  re_ast_free(ast);
  free(ast);
}

TEST(test_re_dot) {
  const char* src = ".";
  ReToken tokens[] = {{TOK_RE_DOT, 0, 1}};
  ReAstNode* ast = re_ast_build_re(src, tokens, 1, NULL, 0);
  assert(ast->kind == RE_AST_DOT);
  re_ast_free(ast);
  free(ast);
}

TEST(test_re_seq) {
  const char* src = "ab";
  ReToken tokens[] = {{TOK_CHAR, 0, 1}, {TOK_CHAR, 1, 2}};
  ReAstNode* ast = re_ast_build_re(src, tokens, 2, NULL, 0);
  assert(ast->kind == RE_AST_SEQ);
  assert((int32_t)darray_size(ast->children) == 2);
  assert(ast->children[0].codepoint == 'a');
  assert(ast->children[1].codepoint == 'b');
  re_ast_free(ast);
  free(ast);
}

TEST(test_re_alt) {
  const char* src = "a|b";
  ReToken tokens[] = {{TOK_CHAR, 0, 1}, {TOK_RE_ALT, 1, 2}, {TOK_CHAR, 2, 3}};
  ReAstNode* ast = re_ast_build_re(src, tokens, 3, NULL, 0);
  assert(ast->kind == RE_AST_ALT);
  assert((int32_t)darray_size(ast->children) == 2);
  assert(ast->children[0].kind == RE_AST_CHAR);
  assert(ast->children[0].codepoint == 'a');
  assert(ast->children[1].codepoint == 'b');
  re_ast_free(ast);
  free(ast);
}

TEST(test_re_quantifier_star) {
  const char* src = "a*";
  ReToken tokens[] = {{TOK_CHAR, 0, 1}, {TOK_RE_STAR, 1, 2}};
  ReAstNode* ast = re_ast_build_re(src, tokens, 2, NULL, 0);
  assert(ast->kind == RE_AST_QUANTIFIED);
  assert(ast->quantifier == '*');
  assert((int32_t)darray_size(ast->children) == 1);
  assert(ast->children[0].kind == RE_AST_CHAR);
  assert(ast->children[0].codepoint == 'a');
  re_ast_free(ast);
  free(ast);
}

TEST(test_re_quantifier_plus) {
  const char* src = "a+";
  ReToken tokens[] = {{TOK_CHAR, 0, 1}, {TOK_RE_PLUS, 1, 2}};
  ReAstNode* ast = re_ast_build_re(src, tokens, 2, NULL, 0);
  assert(ast->kind == RE_AST_QUANTIFIED);
  assert(ast->quantifier == '+');
  re_ast_free(ast);
  free(ast);
}

TEST(test_re_quantifier_maybe) {
  const char* src = "a?";
  ReToken tokens[] = {{TOK_CHAR, 0, 1}, {TOK_RE_MAYBE, 1, 2}};
  ReAstNode* ast = re_ast_build_re(src, tokens, 2, NULL, 0);
  assert(ast->kind == RE_AST_QUANTIFIED);
  assert(ast->quantifier == '?');
  re_ast_free(ast);
  free(ast);
}

TEST(test_re_group) {
  // (a)
  const char* src = "(a)";
  ReToken tokens[] = {{TOK_RE_LPAREN, 0, 1}, {TOK_CHAR, 1, 2}, {TOK_RE_RPAREN, 2, 3}};
  ReAstNode* ast = re_ast_build_re(src, tokens, 3, NULL, 0);
  assert(ast->kind == RE_AST_GROUP);
  assert((int32_t)darray_size(ast->children) == 1);
  assert(ast->children[0].kind == RE_AST_CHAR);
  assert(ast->children[0].codepoint == 'a');
  re_ast_free(ast);
  free(ast);
}

TEST(test_re_shorthand_space) {
  const char* src = "\\s";
  ReToken tokens[] = {{TOK_RE_SPACE_CLASS, 0, 2}};
  ReAstNode* ast = re_ast_build_re(src, tokens, 1, NULL, 0);
  assert(ast->kind == RE_AST_SHORTHAND);
  assert(ast->shorthand == 's');
  re_ast_free(ast);
  free(ast);
}

TEST(test_re_shorthand_word) {
  const char* src = "\\w";
  ReToken tokens[] = {{TOK_RE_WORD_CLASS, 0, 2}};
  ReAstNode* ast = re_ast_build_re(src, tokens, 1, NULL, 0);
  assert(ast->shorthand == 'w');
  re_ast_free(ast);
  free(ast);
}

TEST(test_re_shorthand_digit) {
  const char* src = "\\d";
  ReToken tokens[] = {{TOK_RE_DIGIT_CLASS, 0, 2}};
  ReAstNode* ast = re_ast_build_re(src, tokens, 1, NULL, 0);
  assert(ast->shorthand == 'd');
  re_ast_free(ast);
  free(ast);
}

TEST(test_re_shorthand_hex) {
  const char* src = "\\h";
  ReToken tokens[] = {{TOK_RE_HEX_CLASS, 0, 2}};
  ReAstNode* ast = re_ast_build_re(src, tokens, 1, NULL, 0);
  assert(ast->shorthand == 'h');
  re_ast_free(ast);
  free(ast);
}

TEST(test_re_bof_eof) {
  const char* src = "\\a\\z";
  ReToken tokens[] = {{TOK_RE_BOF, 0, 2}, {TOK_RE_EOF, 2, 4}};
  ReAstNode* ast = re_ast_build_re(src, tokens, 2, NULL, 0);
  assert(ast->kind == RE_AST_SEQ);
  assert(ast->children[0].kind == RE_AST_SHORTHAND);
  assert(ast->children[0].shorthand == 'a');
  assert(ast->children[1].shorthand == 'z');
  re_ast_free(ast);
  free(ast);
}

TEST(test_re_with_charclass) {
  // Simulate a pre-built charclass AST passed via cc_asts
  ReAstNode cc = {.kind = RE_AST_CHARCLASS};
  re_ast_add_child(&cc, (ReAstNode){.kind = RE_AST_CHAR, .codepoint = 'a'});

  const char* src = "";
  ReToken tokens[] = {{RE_AST_TOK_CHARCLASS_BASE + 0, 0, 0}};
  ReAstNode* ast = re_ast_build_re(src, tokens, 1, &cc, 1);
  assert(ast->kind == RE_AST_CHARCLASS);
  assert((int32_t)darray_size(ast->children) == 1);
  assert(ast->children[0].codepoint == 'a');
  re_ast_free(ast);
  free(ast);
}

TEST(test_re_complex) {
  // a(b|c)+
  const char* src = "a(b|c)+";
  ReToken tokens[] = {
      {TOK_CHAR, 0, 1},       // a
      {TOK_RE_LPAREN, 1, 2},  // (
      {TOK_CHAR, 2, 3},       // b
      {TOK_RE_ALT, 3, 4},     // |
      {TOK_CHAR, 4, 5},       // c
      {TOK_RE_RPAREN, 5, 6},  // )
      {TOK_RE_PLUS, 6, 7},    // +
  };
  ReAstNode* ast = re_ast_build_re(src, tokens, 7, NULL, 0);
  assert(ast->kind == RE_AST_SEQ);
  assert((int32_t)darray_size(ast->children) == 2);
  // first child: 'a'
  assert(ast->children[0].kind == RE_AST_CHAR);
  assert(ast->children[0].codepoint == 'a');
  // second child: quantified group
  assert(ast->children[1].kind == RE_AST_QUANTIFIED);
  assert(ast->children[1].quantifier == '+');
  ReAstNode* group = &ast->children[1].children[0];
  assert(group->kind == RE_AST_GROUP);
  ReAstNode* alt = &group->children[0];
  assert(alt->kind == RE_AST_ALT);
  assert((int32_t)darray_size(alt->children) == 2);
  assert(alt->children[0].codepoint == 'b');
  assert(alt->children[1].codepoint == 'c');
  re_ast_free(ast);
  free(ast);
}

TEST(test_re_empty) {
  ReAstNode* ast = re_ast_build_re("", NULL, 0, NULL, 0);
  assert(ast->kind == RE_AST_SEQ);
  assert(darray_size(ast->children) == 0);
  re_ast_free(ast);
  free(ast);
}

int main(void) {
  printf("test_re_ast:\n");

  RUN(test_free_null);
  RUN(test_free_leaf);
  RUN(test_free_with_children);

  RUN(test_clone_null);
  RUN(test_clone_leaf);
  RUN(test_clone_deep);

  RUN(test_add_child);

  RUN(test_build_literal_ascii);
  RUN(test_build_literal_offset);
  RUN(test_build_literal_utf8);

  RUN(test_parse_char_plain);
  RUN(test_parse_char_c_escape_n);
  RUN(test_parse_char_c_escape_t);
  RUN(test_parse_char_codepoint);
  RUN(test_parse_char_codepoint_4digit);
  RUN(test_parse_char_plain_escape);

  RUN(test_charclass_single);
  RUN(test_charclass_negated);
  RUN(test_charclass_range);

  RUN(test_re_single_char);
  RUN(test_re_dot);
  RUN(test_re_seq);
  RUN(test_re_alt);
  RUN(test_re_quantifier_star);
  RUN(test_re_quantifier_plus);
  RUN(test_re_quantifier_maybe);
  RUN(test_re_group);
  RUN(test_re_shorthand_space);
  RUN(test_re_shorthand_word);
  RUN(test_re_shorthand_digit);
  RUN(test_re_shorthand_hex);
  RUN(test_re_bof_eof);
  RUN(test_re_with_charclass);
  RUN(test_re_complex);
  RUN(test_re_empty);

  printf("all ok\n");
  return 0;
}
