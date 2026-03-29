// Regexp AST: construction, parsing from token streams, and utilities.

#include "re_ast.h"
#include "darray.h"
#include "parse.h"
#include "ustr.h"

#include <stdlib.h>
#include <string.h>

// --- Free / clone / helpers ---

void re_ast_free(ReAstNode* node) {
  if (!node) {
    return;
  }
  for (int32_t i = 0; i < (int32_t)darray_size(node->children); i++) {
    re_ast_free(&node->children[i]);
  }
  darray_del(node->children);
}

static ReAstNode _clone_node(ReAstNode* src) {
  ReAstNode dst = *src;
  if ((int32_t)darray_size(src->children) > 0) {
    dst.children = darray_new(sizeof(ReAstNode), darray_size(src->children));
    for (int32_t i = 0; i < (int32_t)darray_size(src->children); i++) {
      dst.children[i] = _clone_node(&src->children[i]);
    }
  } else {
    dst.children = NULL;
  }
  return dst;
}

ReAstNode* re_ast_clone(ReAstNode* src) {
  if (!src) {
    return NULL;
  }
  ReAstNode* dst = malloc(sizeof(ReAstNode));
  *dst = _clone_node(src);
  return dst;
}

void re_ast_add_child(ReAstNode* parent, ReAstNode child) {
  if (!parent->children) {
    parent->children = darray_new(sizeof(ReAstNode), 0);
  }
  darray_push(parent->children, child);
}

ReAstNode* re_ast_build_literal(const char* src, int32_t off, int32_t len) {
  ReAstNode* ast = calloc(1, sizeof(ReAstNode));
  ast->kind = RE_AST_SEQ;
  for (int32_t i = 0; i < len;) {
    int32_t adv;
    int32_t cp = ustr_decode_cp((const uint8_t*)src + off + i, &adv);
    re_ast_add_child(ast, (ReAstNode){.kind = RE_AST_CHAR, .codepoint = cp});
    i += adv;
  }
  return ast;
}

// --- Character codepoint parsing from a single token ---

int32_t re_ast_parse_char_cp(const char* src, ReToken* t) {
  int32_t len = t->end - t->start;
  if (t->id == TOK_CODEPOINT) {
    // \u{XXXX} — skip "\u{", parse hex until '}'
    const char* hex = src + t->start + 3;
    return (int32_t)strtol(hex, NULL, 16);
  }
  if (t->id == TOK_C_ESCAPE) {
    char c = src[t->start + 1];
    switch (c) {
    case 'n':
      return '\n';
    case 'r':
      return '\r';
    case 't':
      return '\t';
    case 'b':
      return '\b';
    case 'f':
      return '\f';
    case 'v':
      return '\v';
    default:
      return c;
    }
  }
  if (t->id == TOK_PLAIN_ESCAPE) {
    if (len >= 2) {
      int32_t adv;
      return ustr_decode_cp((const uint8_t*)src + t->start + 1, &adv);
    }
    return 0;
  }
  int32_t adv;
  return ustr_decode_cp((const uint8_t*)src + t->start, &adv);
}

// --- Build charclass AST from token array ---

ReAstNode re_ast_build_charclass(const char* src, ReToken* tokens, int32_t ntokens, bool negated) {
  ReAstNode node = {.kind = RE_AST_CHARCLASS, .negated = negated};
  int32_t i = 0;
  while (i < ntokens) {
    int32_t lo = re_ast_parse_char_cp(src, &tokens[i]);
    i++;
    if (i < ntokens && tokens[i].id == TOK_RANGE_SEP) {
      i++;
      if (i < ntokens) {
        int32_t hi = re_ast_parse_char_cp(src, &tokens[i]);
        re_ast_add_child(&node, (ReAstNode){.kind = RE_AST_RANGE, .range_lo = lo, .range_hi = hi});
        i++;
      }
    } else {
      re_ast_add_child(&node, (ReAstNode){.kind = RE_AST_CHAR, .codepoint = lo});
    }
  }
  return node;
}

// --- Recursive descent regex parser ---

typedef struct {
  const char* src;
  ReToken* tokens;
  int32_t ntokens;
  int32_t pos;
  ReAstNode* cc_asts;
  int32_t ncc;
} ReParser;

static ReAstNode _parse_alt(ReParser* rp);

static ReToken* _peek(ReParser* rp) {
  if (rp->pos < rp->ntokens) {
    return &rp->tokens[rp->pos];
  }
  return NULL;
}

static ReToken* _next(ReParser* rp) {
  if (rp->pos < rp->ntokens) {
    return &rp->tokens[rp->pos++];
  }
  return NULL;
}

static bool _is_atom(ReParser* rp, ReToken* t) {
  if (!t) {
    return false;
  }
  if (t->id >= RE_AST_TOK_CHARCLASS_BASE && t->id < RE_AST_TOK_CHARCLASS_BASE + rp->ncc) {
    return true;
  }
  int32_t id = t->id;
  return id == TOK_RE_DOT || id == TOK_RE_SPACE_CLASS || id == TOK_RE_WORD_CLASS || id == TOK_RE_DIGIT_CLASS ||
         id == TOK_RE_HEX_CLASS || id == TOK_RE_BOF || id == TOK_RE_EOF || id == TOK_CHAR || id == TOK_CODEPOINT ||
         id == TOK_C_ESCAPE || id == TOK_PLAIN_ESCAPE || id == TOK_RE_LPAREN;
}

static ReAstNode _parse_atom(ReParser* rp) {
  ReToken* t = _peek(rp);
  if (!t) {
    return (ReAstNode){.kind = RE_AST_SEQ};
  }

  if (t->id >= RE_AST_TOK_CHARCLASS_BASE && t->id < RE_AST_TOK_CHARCLASS_BASE + rp->ncc) {
    _next(rp);
    int32_t idx = t->id - RE_AST_TOK_CHARCLASS_BASE;
    ReAstNode copy = rp->cc_asts[idx];
    rp->cc_asts[idx].children = NULL;
    return copy;
  }

  if (t->id == TOK_RE_LPAREN) {
    _next(rp);
    ReAstNode inner = _parse_alt(rp);
    ReToken* close = _peek(rp);
    if (close && close->id == TOK_RE_RPAREN) {
      _next(rp);
    }
    ReAstNode group = {.kind = RE_AST_GROUP};
    re_ast_add_child(&group, inner);
    return group;
  }

  _next(rp);

  if (t->id == TOK_RE_DOT) {
    return (ReAstNode){.kind = RE_AST_DOT};
  }
  if (t->id == TOK_RE_SPACE_CLASS) {
    return (ReAstNode){.kind = RE_AST_SHORTHAND, .shorthand = 's'};
  }
  if (t->id == TOK_RE_WORD_CLASS) {
    return (ReAstNode){.kind = RE_AST_SHORTHAND, .shorthand = 'w'};
  }
  if (t->id == TOK_RE_DIGIT_CLASS) {
    return (ReAstNode){.kind = RE_AST_SHORTHAND, .shorthand = 'd'};
  }
  if (t->id == TOK_RE_HEX_CLASS) {
    return (ReAstNode){.kind = RE_AST_SHORTHAND, .shorthand = 'h'};
  }
  if (t->id == TOK_RE_BOF) {
    return (ReAstNode){.kind = RE_AST_SHORTHAND, .shorthand = 'a'};
  }
  if (t->id == TOK_RE_EOF) {
    return (ReAstNode){.kind = RE_AST_SHORTHAND, .shorthand = 'z'};
  }

  return (ReAstNode){.kind = RE_AST_CHAR, .codepoint = re_ast_parse_char_cp(rp->src, t)};
}

static ReAstNode _parse_quantified(ReParser* rp) {
  ReAstNode atom = _parse_atom(rp);
  ReToken* t = _peek(rp);
  if (t && (t->id == TOK_RE_MAYBE || t->id == TOK_RE_PLUS || t->id == TOK_RE_STAR)) {
    _next(rp);
    int32_t q = (t->id == TOK_RE_MAYBE) ? '?' : (t->id == TOK_RE_PLUS) ? '+' : '*';
    ReAstNode quant = {.kind = RE_AST_QUANTIFIED, .quantifier = q};
    re_ast_add_child(&quant, atom);
    return quant;
  }
  return atom;
}

static ReAstNode _parse_seq(ReParser* rp) {
  ReAstNode seq = {.kind = RE_AST_SEQ};
  while (_is_atom(rp, _peek(rp))) {
    re_ast_add_child(&seq, _parse_quantified(rp));
  }
  if (darray_size(seq.children) == 1) {
    ReAstNode single = seq.children[0];
    darray_del(seq.children);
    return single;
  }
  return seq;
}

static ReAstNode _parse_alt(ReParser* rp) {
  ReAstNode first = _parse_seq(rp);
  ReToken* t = _peek(rp);
  if (!t || t->id != TOK_RE_ALT) {
    return first;
  }
  ReAstNode alt = {.kind = RE_AST_ALT};
  re_ast_add_child(&alt, first);
  while (t && t->id == TOK_RE_ALT) {
    _next(rp);
    re_ast_add_child(&alt, _parse_seq(rp));
    t = _peek(rp);
  }
  return alt;
}

ReAstNode* re_ast_build_re(const char* src, ReToken* tokens, int32_t ntokens, ReAstNode* cc_asts, int32_t ncc_asts) {
  ReParser rp = {.src = src, .tokens = tokens, .ntokens = ntokens, .pos = 0, .cc_asts = cc_asts, .ncc = ncc_asts};
  ReAstNode* result = calloc(1, sizeof(ReAstNode));
  *result = _parse_alt(&rp);
  return result;
}
