// reference: parse_gen.md
#include "irwriter.h"
#include "parse.h"
#include "re.h"

#include <stdio.h>
#include <stdlib.h>

// --- Scope builders ---

// *noise: /#[^\n]*/ @comment, /[ \t]+/ @space, /\n+/ @nl
static void _build_noise(ReLex* l) {
  re_lex_add(l, "#[^\\n]*", __LINE__, 15, ACTION_IGNORE); // @comment
  re_lex_add(l, "[ \\t]+", __LINE__, 15, ACTION_IGNORE);  // @space
  re_lex_add(l, "\\n+", __LINE__, 15, TOK_NL);            // @nl
}

// *chars: /\u\{\h+\}/ @codepoint, /\\[bfnrtv]/ @c_escape, /\\./ @plain_escape, /./ @char
static void _build_chars(ReLex* l) {
  re_lex_add(l, "\\\\u\\{[0-9a-fA-F]+\\}", __LINE__, 15, TOK_CODEPOINT);
  re_lex_add(l, "\\\\[bfnrtv]", __LINE__, 15, TOK_C_ESCAPE);
  re_lex_add(l, "\\\\.", __LINE__, 15, TOK_PLAIN_ESCAPE);
  re_lex_add(l, ".", __LINE__, 15, TOK_CHAR);
}

// *vpa_commons: hooks, ids, re, str
static void _build_vpa_commons(ReLex* l) {
  re_lex_add(l, "\\.begin", __LINE__, 15, TOK_HOOK_BEGIN);
  re_lex_add(l, "\\.end", __LINE__, 15, TOK_HOOK_END);
  re_lex_add(l, "\\.fail", __LINE__, 15, TOK_HOOK_FAIL);
  re_lex_add(l, "\\.unparse", __LINE__, 15, TOK_HOOK_UNPARSE);
  re_lex_add(l, "\\.noop", __LINE__, 15, TOK_HOOK_NOOP);

  re_lex_add(l, "[a-z_][a-zA-Z0-9_]*", __LINE__, 15, TOK_VPA_ID);
  re_lex_add(l, "EOF", __LINE__, 15, TOK_PSEUDO_FRAG_EOF);
  re_lex_add(l, "[A-Z][a-zA-Z0-9_]*", __LINE__, 15, TOK_RE_FRAG_ID);
  re_lex_add(l, "\\*[a-z_][a-zA-Z0-9_]*", __LINE__, 15, TOK_MODULE_ID);
  re_lex_add(l, "\\.[a-z_][a-zA-Z0-9_]*", __LINE__, 15, TOK_USER_HOOK_ID);
  re_lex_add(l, "@[a-z_][a-zA-Z0-9_]*", __LINE__, 15, TOK_TOK_ID);

  // re = /(b|i|ib|bi)?\// .set_re_mode .begin
  re_lex_add(l, "(b|i|ib|bi)?/", __LINE__, 15, ACTION_SET_RE_MODE_BEGIN);
  // re_str = /["']/ .set_quote .begin
  re_lex_add(l, "[\"']", __LINE__, 15, ACTION_SET_QUOTE_BEGIN);
}

// *directive_keywords: "%ignore" "%effect" "%define" "=" "|"
static void _build_directive_keywords(ReLex* l) {
  re_lex_add(l, "%ignore", __LINE__, 15, LIT_IGNORE);
  re_lex_add(l, "%effect", __LINE__, 15, LIT_EFFECT);
  re_lex_add(l, "%define", __LINE__, 15, LIT_DEFINE);
  re_lex_add(l, "=", __LINE__, 15, LIT_EQ);
  re_lex_add(l, "\\|", __LINE__, 15, LIT_OR);
}

// *re_ops: "|" "(" ")" "?" "+" "*"
static void _build_re_ops(ReLex* l) {
  re_lex_add(l, "\\|", __LINE__, 15, LIT_OR);
  re_lex_add(l, "\\(", __LINE__, 15, LIT_LPAREN);
  re_lex_add(l, "\\)", __LINE__, 15, LIT_RPAREN);
  re_lex_add(l, "\\?", __LINE__, 15, LIT_QUESTION);
  re_lex_add(l, "\\+", __LINE__, 15, LIT_PLUS);
  re_lex_add(l, "\\*", __LINE__, 15, LIT_STAR);
}

// *peg_ops: "<" ">" "?" "+" "*" "&" "!"
static void _build_peg_ops(ReLex* l) {
  re_lex_add(l, "<", __LINE__, 15, LIT_INTERLACE_BEGIN);
  re_lex_add(l, ">", __LINE__, 15, LIT_INTERLACE_END);
  re_lex_add(l, "\\?", __LINE__, 15, LIT_QUESTION);
  re_lex_add(l, "\\+", __LINE__, 15, LIT_PLUS);
  re_lex_add(l, "\\*", __LINE__, 15, LIT_STAR);
  re_lex_add(l, "&", __LINE__, 15, LIT_AND);
  re_lex_add(l, "!", __LINE__, 15, LIT_NOT);
}

// main = { vpa peg *noise }
static ReLex* _build_main_scope(void) {
  ReLex* l = re_lex_new("lex_main", "nest", "");

  re_lex_add(l, "\\[\\[vpa\\]\\]", __LINE__, 15, SCOPE_VPA);
  re_lex_add(l, "\\[\\[peg\\]\\]", __LINE__, 15, SCOPE_PEG);
  _build_noise(l);
  return l;
}

// vpa = /\s*\[\[vpa\]\]\n/ .begin { ... }
static ReLex* _build_vpa_scope(void) {
  ReLex* l = re_lex_new("lex_vpa", "nest", "");

  // /\[\[peg\]\]/ .unparse .end
  re_lex_add(l, "\\[\\[peg\\]\\]", __LINE__, 15, ACTION_UNPARSE_END);
  // *directive_keywords
  _build_directive_keywords(l);
  // *vpa_commons
  _build_vpa_commons(l);
  // scope = "{" .begin
  re_lex_add(l, "\\{", __LINE__, 15, SCOPE_SCOPE);
  // lit_scope = "@{" .begin
  re_lex_add(l, "@\\{", __LINE__, 15, SCOPE_LIT_SCOPE);
  // *noise
  _build_noise(l);
  return l;
}

// scope = "{" .begin { *vpa_commons "}" .end *noise }
static ReLex* _build_scope_scope(void) {
  ReLex* l = re_lex_new("lex_scope", "nest", "");

  // *vpa_commons
  _build_vpa_commons(l);
  // "}" .end
  re_lex_add(l, "\\}", __LINE__, 15, ACTION_END);
  // *noise
  _build_noise(l);
  return l;
}

// lit_scope = "@{" .begin { re_str "}" .end *noise }
static ReLex* _build_lit_scope_scope(void) {
  ReLex* l = re_lex_new("lex_lit_scope", "nest", "");

  // re_str = /["']/ .set_quote .begin
  re_lex_add(l, "[\"']", __LINE__, 15, ACTION_SET_QUOTE_BEGIN);
  // "}" .end
  re_lex_add(l, "\\}", __LINE__, 15, ACTION_END);
  // *noise
  _build_noise(l);
  return l;
}

// re = /(b|i|ib|bi)?\// @re_tag .begin { ... }
static ReLex* _build_re_scope(void) {
  ReLex* l = re_lex_new("lex_re", "nest", "");

  // /\// .end
  re_lex_add(l, "/", __LINE__, 15, ACTION_END);
  // charclass = /\[\^?/ .set_cc_kind .begin
  re_lex_add(l, "\\[\\^", __LINE__, 15, ACTION_SET_CC_KIND_BEGIN);
  re_lex_add(l, "\\[", __LINE__, 15, ACTION_SET_CC_KIND_BEGIN);
  re_lex_add(l, "\\.", __LINE__, 15, TOK_RE_DOT);
  re_lex_add(l, "\\\\s", __LINE__, 15, TOK_RE_SPACE_CLASS);
  re_lex_add(l, "\\\\w", __LINE__, 15, TOK_RE_WORD_CLASS);
  re_lex_add(l, "\\\\d", __LINE__, 15, TOK_RE_DIGIT_CLASS);
  re_lex_add(l, "\\\\h", __LINE__, 15, TOK_RE_HEX_CLASS);
  // re_ref = /#\{/ .begin
  re_lex_add(l, "#\\{", __LINE__, 15, SCOPE_RE_REF);
  // *re_ops
  _build_re_ops(l);
  // *chars
  _build_chars(l);

  return l;
}

// charclass = /\[\^?/ .set_cc_kind .begin { "/" .unparse .end /\]/ .end /-/ @range_sep *chars }
static ReLex* _build_charclass_scope(void) {
  ReLex* l = re_lex_new("lex_charclass", "nest", "");

  re_lex_add(l, "/", __LINE__, 15, ACTION_UNPARSE_END);
  re_lex_add(l, "\\]", __LINE__, 15, ACTION_END);
  re_lex_add(l, "-", __LINE__, 15, TOK_RANGE_SEP);
  _build_chars(l);

  return l;
}

// re_ref = /#\{/ .begin { ID @re_ref /\}/ .end }
static ReLex* _build_re_ref_scope(void) {
  ReLex* l = re_lex_new("lex_re_ref", "nest", "");

  re_lex_add(l, "[A-Z][a-zA-Z0-9_]*", __LINE__, 15, TOK_RE_REF);
  re_lex_add(l, "\\}", __LINE__, 15, ACTION_END);

  return l;
}

// peg = /\s*\[\[peg\]\]\n/ .begin { ... }
static ReLex* _build_peg_scope(void) {
  ReLex* l = re_lex_new("lex_peg", "nest", "");

  // "=" @peg_assign
  re_lex_add(l, "=", __LINE__, 15, LIT_EQ);
  // *peg_commons
  re_lex_add(l, "[a-z_][a-zA-Z0-9_]*", __LINE__, 15, TOK_PEG_ID);
  re_lex_add(l, "@[a-z_][a-zA-Z0-9_]*", __LINE__, 15, TOK_PEG_TOK_ID);
  // peg_str = /["']/ .set_quote .begin
  re_lex_add(l, "[\"']", __LINE__, 15, ACTION_SET_QUOTE_BEGIN);
  _build_peg_ops(l);
  // branches = "[" .begin
  re_lex_add(l, "\\[", __LINE__, 15, SCOPE_BRANCHES);
  // *noise
  _build_noise(l);
  // EOF .end — handled specially in _lex_scope, not by DFA
  return l;
}

// branches = "[" .begin { *peg_commons peg_tag "]" .end *noise }
static ReLex* _build_branches_scope(void) {
  ReLex* l = re_lex_new("lex_branches", "nest", "");

  // *peg_commons
  re_lex_add(l, "[a-z_][a-zA-Z0-9_]*", __LINE__, 15, TOK_PEG_ID);
  re_lex_add(l, "@[a-z_][a-zA-Z0-9_]*", __LINE__, 15, TOK_PEG_TOK_ID);
  re_lex_add(l, "[\"']", __LINE__, 15, ACTION_SET_QUOTE_BEGIN);
  _build_peg_ops(l);
  // peg_tag = /:/ .begin
  re_lex_add(l, ":", __LINE__, 15, SCOPE_PEG_TAG);
  // "]" .end
  re_lex_add(l, "\\]", __LINE__, 15, ACTION_END);
  // *noise
  _build_noise(l);
  return l;
}

// peg_tag = /:/ .begin { ID @tag_id /\n/ .end @nl *noise }
// tag_id must appear on the same line after colon
static ReLex* _build_peg_tag_scope(void) {
  ReLex* l = re_lex_new("lex_peg_tag", "nest", "");

  re_lex_add(l, "[a-z_][a-zA-Z0-9_]*", __LINE__, 15, TOK_TAG_ID);
  re_lex_add(l, "\\n", __LINE__, 15, ACTION_END_NL);
  _build_noise(l);

  return l;
}

// re_str = /["']/ .set_quote .begin { /["']/ .str_check_end *chars }
static ReLex* _build_re_str_scope(void) {
  ReLex* l = re_lex_new("lex_re_str", "nest", "");

  re_lex_add(l, "[\"']", __LINE__, 15, ACTION_STR_CHECK_END);
  _build_chars(l);

  return l;
}

// peg_str = /["']/ .set_quote .begin { /["']/ .str_check_end *chars }
static ReLex* _build_peg_str_scope(void) {
  ReLex* l = re_lex_new("lex_peg_str", "nest", "");

  re_lex_add(l, "[\"']", __LINE__, 15, ACTION_STR_CHECK_END);
  _build_chars(l);

  return l;
}

int main(int argc, char** argv) {
  const char* output = "build/nest_lex.ll";
  if (argc > 1) {
    output = argv[1];
  }

  FILE* f = fopen(output, "w");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", output);
    return 1;
  }

  IrWriter* w = irwriter_new(f);
  irwriter_gen_rt_simple(w);

  // order must match ScopeId enum: MAIN, VPA, SCOPE, LIT_SCOPE, PEG, BRANCHES, PEG_TAG, RE, RE_REF, CHARCLASS, RE_STR,
  // PEG_STR
  ReLex* scopes[] = {
      _build_main_scope(),   _build_vpa_scope(),       _build_scope_scope(),   _build_lit_scope_scope(),
      _build_peg_scope(),    _build_branches_scope(),  _build_peg_tag_scope(), _build_re_scope(),
      _build_re_ref_scope(), _build_charclass_scope(), _build_re_str_scope(),  _build_peg_str_scope(),
  };
  int32_t nscopes = sizeof(scopes) / sizeof(scopes[0]);

  for (int32_t i = 0; i < nscopes; i++) {
    re_lex_gen(scopes[i], w, false);
    re_lex_del(scopes[i]);
  }

  irwriter_end(w);
  irwriter_del(w);
  fclose(f);

  printf("generated %s\n", output);
  return 0;
}
