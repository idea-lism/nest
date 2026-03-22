#pragma once

#include <stdbool.h>
#include <stdint.h>

// --- Regexp AST node types ---

typedef enum {
  RE_AST_CHAR,
  RE_AST_RANGE,
  RE_AST_DOT,
  RE_AST_SHORTHAND,
  RE_AST_CHARCLASS,
  RE_AST_ALT,
  RE_AST_GROUP,
  RE_AST_QUANTIFIED,
  RE_AST_SEQ,
} ReAstKind;

typedef struct ReAstNode ReAstNode;

struct ReAstNode {
  ReAstKind kind;
  int32_t codepoint;
  int32_t range_lo;
  int32_t range_hi;
  int32_t shorthand;   // 's','w','d','h','a','z'
  bool negated;        // RE_AST_CHARCLASS
  int32_t quantifier;  // '?','+','*', or 0
  ReAstNode* children; // darray
};

// --- Token representation for regex parsing ---

typedef struct {
  int32_t id;
  int32_t start;
  int32_t end;
} ReToken;

// --- Public API ---

#define RE_AST_TOK_CHARCLASS_BASE 10000

void re_ast_free(ReAstNode* node);
ReAstNode* re_ast_clone(ReAstNode* src);
void re_ast_add_child(ReAstNode* parent, ReAstNode child);
ReAstNode* re_ast_build_literal(const char* src, int32_t off, int32_t len);

int32_t re_ast_parse_char_cp(const char* src, ReToken* t);
ReAstNode re_ast_build_charclass(const char* src, ReToken* tokens, int32_t ntokens, bool negated);
ReAstNode* re_ast_build_re(const char* src, ReToken* tokens, int32_t ntokens, ReAstNode* cc_asts, int32_t ncc_asts);
