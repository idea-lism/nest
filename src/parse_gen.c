// Build-time tool: generates DFA lexers for the .nest syntax.
// Mimics the structure of specs/bootstrap.nest with hand-written _lex_xxx calls.
// Produces a single LLVM IR module with one function per scope.

#include "irwriter.h"
#include "parse.h"
#include "re.h"
#include "ustr.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Lex helpers (inlined from lex.c) ---

typedef struct {
  Aut* aut;
  Re* re;
  const char* source_file;
  bool started;
  bool icase;
  bool binary;
} Lex;

#define LEX_ERR_PAREN (-1)
#define LEX_ERR_BRACKET (-2)

typedef struct {
  Lex* lex;
  int32_t* cps;
  int32_t ncp;
  int32_t pos;
  bool binary;
  bool icase;
  int32_t line;
  int32_t col;
  char* ustr_buf;
  int32_t err;
} LexParser;

static void _lp_parse_expr(LexParser* p);

static int32_t _lp_peek(LexParser* p) {
  if (p->pos >= p->ncp) {
    return -1;
  }
  return p->cps[p->pos];
}

static int32_t _lp_advance(LexParser* p) {
  if (p->pos >= p->ncp) {
    return -1;
  }
  return p->cps[p->pos++];
}

static DebugInfo _lp_di(LexParser* p) { return (DebugInfo){p->line, p->col + p->pos}; }

static void _range_add_icase(ReRange* range, int32_t start, int32_t end, bool icase) {
  re_range_add(range, start, end);
  if (!icase) {
    return;
  }
  if (start <= 'z' && end >= 'a') {
    int32_t lo = start < 'a' ? 'a' : start;
    int32_t hi = end > 'z' ? 'z' : end;
    re_range_add(range, lo - 32, hi - 32);
  }
  if (start <= 'Z' && end >= 'A') {
    int32_t lo = start < 'A' ? 'A' : start;
    int32_t hi = end > 'Z' ? 'Z' : end;
    re_range_add(range, lo + 32, hi + 32);
  }
}

static void _lp_emit_ch(LexParser* p, int32_t cp) {
  Re* re = p->lex->re;
  DebugInfo di = _lp_di(p);
  if (p->icase && ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'))) {
    ReRange* range = re_range_new();
    _range_add_icase(range, cp, cp, true);
    re_append_range(re, range, di);
    re_range_del(range);
  } else {
    re_append_ch(re, cp, di);
  }
}

static void _add_class_ranges(ReRange* range, int32_t cls) {
  switch (cls) {
  case 's':
    re_range_add(range, '\t', '\r');
    re_range_add(range, ' ', ' ');
    break;
  case 'w':
    re_range_add(range, '0', '9');
    re_range_add(range, 'A', 'Z');
    re_range_add(range, '_', '_');
    re_range_add(range, 'a', 'z');
    break;
  case 'd':
    re_range_add(range, '0', '9');
    break;
  case 'h':
    re_range_add(range, '0', '9');
    re_range_add(range, 'A', 'F');
    re_range_add(range, 'a', 'f');
    break;
  }
}

static int32_t _c_escape(int32_t ch) {
  switch (ch) {
  case 'n':
    return '\n';
  case 't':
    return '\t';
  case 'r':
    return '\r';
  case '0':
    return '\0';
  case 'f':
    return '\f';
  case 'v':
    return '\v';
  default:
    return -1;
  }
}

static int32_t _lp_parse_unicode_escape(LexParser* p) {
  _lp_advance(p);
  int32_t cp = 0;
  while (_lp_peek(p) != '}' && _lp_peek(p) >= 0) {
    int32_t ch = _lp_advance(p);
    if (ch >= '0' && ch <= '9') {
      cp = cp * 16 + (ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      cp = cp * 16 + (ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      cp = cp * 16 + (ch - 'A' + 10);
    } else {
      fprintf(stderr, "parse_gen: invalid hex char '%c' in \\u{...} escape\n", (char)ch);
      exit(1);
    }
  }
  if (_lp_peek(p) == '}') {
    _lp_advance(p);
  }
  return cp;
}

static int32_t _lp_parse_class_escape(LexParser* p, ReRange* range) {
  int32_t ch = _lp_advance(p);
  if (ch < 0) {
    return -1;
  }
  int32_t esc = _c_escape(ch);
  if (esc >= 0) {
    return esc;
  }
  switch (ch) {
  case 'u':
    return _lp_parse_unicode_escape(p);
  case 's':
  case 'w':
  case 'd':
  case 'h':
    _add_class_ranges(range, ch);
    return -1;
  default:
    return ch;
  }
}

static void _lp_parse_charclass(LexParser* p) {
  if (p->err) {
    return;
  }
  _lp_advance(p);

  bool neg = false;
  if (_lp_peek(p) == '^') {
    _lp_advance(p);
    neg = true;
  }

  ReRange* range = re_range_new();

  while (_lp_peek(p) != ']' && _lp_peek(p) >= 0 && !p->err) {
    int32_t cp;
    if (_lp_peek(p) == '\\') {
      _lp_advance(p);
      cp = _lp_parse_class_escape(p, range);
    } else {
      cp = _lp_advance(p);
    }

    if (cp >= 0 && _lp_peek(p) == '-' && p->pos + 1 < p->ncp && p->cps[p->pos + 1] != ']') {
      _lp_advance(p);
      int32_t cp2;
      if (_lp_peek(p) == '\\') {
        _lp_advance(p);
        cp2 = _lp_parse_class_escape(p, range);
      } else {
        cp2 = _lp_advance(p);
      }
      if (cp2 >= 0) {
        _range_add_icase(range, cp, cp2, p->icase);
      } else {
        _range_add_icase(range, cp, cp, p->icase);
      }
    } else if (cp >= 0) {
      _range_add_icase(range, cp, cp, p->icase);
    }
  }

  if (_lp_peek(p) != ']') {
    p->err = LEX_ERR_BRACKET;
    re_range_del(range);
    return;
  }
  _lp_advance(p);

  if (neg) {
    re_range_neg(range);
  }

  re_append_range(p->lex->re, range, _lp_di(p));
  re_range_del(range);
}

static void _lp_parse_atom(LexParser* p) {
  if (p->err) {
    return;
  }

  int32_t ch = _lp_peek(p);

  if (ch == '(') {
    _lp_advance(p);
    re_lparen(p->lex->re);
    _lp_parse_expr(p);
    if (p->err) {
      return;
    }
    if (_lp_peek(p) != ')') {
      p->err = LEX_ERR_PAREN;
      return;
    }
    _lp_advance(p);
    re_rparen(p->lex->re);
  } else if (ch == '[') {
    _lp_parse_charclass(p);
  } else if (ch == '.') {
    _lp_advance(p);
    ReRange* range = re_range_new();
    if (p->binary) {
      re_range_add(range, 0, 9);
      re_range_add(range, 11, 255);
    } else {
      re_range_add(range, 0, 9);
      re_range_add(range, 11, 0x10FFFF);
    }
    re_append_range(p->lex->re, range, _lp_di(p));
    re_range_del(range);
  } else if (ch == '\\') {
    _lp_advance(p);
    ch = _lp_peek(p);
    if (ch < 0) {
      return;
    }
    if (ch == 'a') {
      _lp_advance(p);
      re_append_ch(p->lex->re, LEX_CP_BOF, _lp_di(p));
    } else if (ch == 'z') {
      _lp_advance(p);
      re_append_ch(p->lex->re, LEX_CP_EOF, _lp_di(p));
    } else if (ch == 's' || ch == 'w' || ch == 'd' || ch == 'h') {
      _lp_advance(p);
      ReRange* range = re_range_new();
      _add_class_ranges(range, ch);
      re_append_range(p->lex->re, range, _lp_di(p));
      re_range_del(range);
    } else if (ch == 'u') {
      _lp_advance(p);
      int32_t cp = _lp_parse_unicode_escape(p);
      _lp_emit_ch(p, cp);
    } else {
      int32_t esc = _c_escape(ch);
      _lp_advance(p);
      _lp_emit_ch(p, esc >= 0 ? esc : ch);
    }
  } else {
    _lp_advance(p);
    _lp_emit_ch(p, ch);
  }
}

static void _lp_parse_quantified(LexParser* p) {
  if (p->err) {
    return;
  }

  Re* re = p->lex->re;
  Aut* aut = p->lex->aut;
  int32_t before = re_cur_state(re);

  _lp_parse_atom(p);
  if (p->err) {
    return;
  }

  int32_t ch = _lp_peek(p);
  if (ch == '?') {
    _lp_advance(p);
    aut_epsilon(aut, before, re_cur_state(re));
  } else if (ch == '+') {
    _lp_advance(p);
    aut_epsilon(aut, re_cur_state(re), before);
  } else if (ch == '*') {
    _lp_advance(p);
    aut_epsilon(aut, before, re_cur_state(re));
    aut_epsilon(aut, re_cur_state(re), before);
  }
}

static void _lp_parse_branch(LexParser* p) {
  while (!p->err) {
    int32_t ch = _lp_peek(p);
    if (ch < 0 || ch == '|' || ch == ')') {
      break;
    }
    _lp_parse_quantified(p);
  }
}

static void _lp_parse_expr(LexParser* p) {
  if (p->err) {
    return;
  }
  _lp_parse_branch(p);
  while (_lp_peek(p) == '|' && !p->err) {
    _lp_advance(p);
    re_fork(p->lex->re);
    _lp_parse_branch(p);
  }
}

static Lex* _lex_new(const char* func_name, const char* source_file_name, const char* mode) {
  Lex* l = calloc(1, sizeof(Lex));
  l->aut = aut_new(func_name, source_file_name);
  l->re = re_new(l->aut);
  l->source_file = source_file_name;
  for (const char* m = mode; *m; m++) {
    if (*m == 'i') {
      l->icase = true;
    }
    if (*m == 'b') {
      l->binary = true;
    }
  }
  re_lparen(l->re);
  return l;
}

static void _lex_del(Lex* l) {
  if (!l) {
    return;
  }
  re_del(l->re);
  aut_del(l->aut);
  free(l);
}

static int32_t _lex_add(Lex* l, const char* pattern, int32_t line, int32_t col, int32_t action_id) {
  if (l->started) {
    re_fork(l->re);
  }
  l->started = true;

  LexParser p = {0};
  p.lex = l;
  p.line = line;
  p.col = col;
  p.icase = l->icase;
  p.binary = l->binary;

  int32_t len = (int32_t)strlen(pattern);
  if (p.binary) {
    p.ncp = len;
    p.cps = malloc((size_t)len * sizeof(int32_t));
    for (int32_t i = 0; i < len; i++) {
      p.cps[i] = (uint8_t)pattern[i];
    }
  } else {
    p.ustr_buf = ustr_new((size_t)len, pattern);
    p.ncp = ustr_size(p.ustr_buf);
    if (p.ncp > 0) {
      p.cps = malloc((size_t)p.ncp * sizeof(int32_t));
      UstrIter it;
      ustr_iter_init(&it, p.ustr_buf, 0);
      for (int32_t i = 0; i < p.ncp; i++) {
        p.cps[i] = ustr_iter_next(&it);
      }
    }
  }

  _lp_parse_expr(&p);

  int32_t err = p.err;
  free(p.cps);
  if (p.ustr_buf) {
    ustr_del(p.ustr_buf);
  }

  if (err) {
    return err;
  }

  re_action(l->re, action_id);
  return action_id;
}

static void _lex_gen_func(Lex* l, IrWriter* w, bool debug_mode) {
  re_rparen(l->re);
  aut_optimize(l->aut);
  aut_gen_dfa(l->aut, w, debug_mode);
}

// --- Scope builders ---

static void _build_ignores(Lex* l) {
  _lex_add(l, "#[^\\n]*", __LINE__, 15, IGNORED_COMMENT);
  _lex_add(l, "[ \\t]+", __LINE__, 15, IGNORED_SPACE);
  _lex_add(l, "\\n+", __LINE__, 15, IGNORED_NL);
}

static Lex* _build_main_scope(void) {
  Lex* l = _lex_new("lex_main", "nest", "");

  _lex_add(l, "\\[\\[vpa\\]\\]", __LINE__, 15, TOK_SECTION_VPA);
  _lex_add(l, "\\[\\[peg\\]\\]", __LINE__, 15, TOK_SECTION_PEG);
  _build_ignores(l);
  return l;
}

static Lex* _build_vpa_scope(void) {
  Lex* l = _lex_new("lex_vpa", "nest", "");

  _lex_add(l, "\\[\\[peg\\]\\]", __LINE__, 15, TOK_SECTION_PEG);

  _lex_add(l, "(b|i|ib|bi)?/", __LINE__, 15, TOK_RE_BEGIN);

  _lex_add(l, "%state", __LINE__, 15, TOK_KW_STATE);
  _lex_add(l, "%ignore", __LINE__, 15, TOK_KW_IGNORE);
  _lex_add(l, "%effect", __LINE__, 15, TOK_KW_EFFECT);
  _lex_add(l, "%keyword", __LINE__, 15, TOK_KW_KEYWORD);

  _lex_add(l, "=", __LINE__, 15, TOK_OPS_EQ);
  _lex_add(l, "\\|", __LINE__, 15, TOK_OPS_PIPE);

  _lex_add(l, "\\.begin", __LINE__, 15, TOK_HOOK_BEGIN);
  _lex_add(l, "\\.end", __LINE__, 15, TOK_HOOK_END);
  _lex_add(l, "\\.fail", __LINE__, 15, TOK_HOOK_FAIL);
  _lex_add(l, "\\.unparse", __LINE__, 15, TOK_HOOK_UNPARSE);

  _lex_add(l, "[a-z_][a-zA-Z0-9_]*", __LINE__, 15, TOK_VPA_ID);
  _lex_add(l, "\\*[a-z_][a-zA-Z0-9_]*", __LINE__, 15, TOK_MACRO_ID);
  _lex_add(l, "\\.[a-z_][a-zA-Z0-9_]*", __LINE__, 15, TOK_USER_HOOK_ID);
  _lex_add(l, "@[a-z_][a-zA-Z0-9_]*", __LINE__, 15, TOK_TOK_ID);
  _lex_add(l, "\\$[a-z_][a-zA-Z0-9_]*", __LINE__, 15, TOK_STATE_ID);

  _lex_add(l, "\\{", __LINE__, 15, TOK_SCOPE_BEGIN);
  _lex_add(l, "\\}", __LINE__, 15, TOK_SCOPE_END);

  _lex_add(l, "\"", __LINE__, 15, TOK_STR_BEGIN);
  _lex_add(l, "'", __LINE__, 15, TOK_STR_BEGIN);

  _build_ignores(l);
  return l;
}

static Lex* _build_re_scope(void) {
  Lex* l = _lex_new("lex_re", "nest", "");

  _lex_add(l, "/", __LINE__, 15, TOK_RE_END);
  _lex_add(l, "\\[\\^", __LINE__, 15, TOK_NEG_CLASS_BEGIN);
  _lex_add(l, "\\[", __LINE__, 15, TOK_CLASS_BEGIN);
  _lex_add(l, "\\.", __LINE__, 15, TOK_RE_DOT);
  _lex_add(l, "\\\\s", __LINE__, 15, TOK_RE_SPACE_CLASS);
  _lex_add(l, "\\\\w", __LINE__, 15, TOK_RE_WORD_CLASS);
  _lex_add(l, "\\\\d", __LINE__, 15, TOK_RE_DIGIT_CLASS);
  _lex_add(l, "\\\\h", __LINE__, 15, TOK_RE_HEX_CLASS);
  _lex_add(l, "\\\\a", __LINE__, 15, TOK_RE_BOF);
  _lex_add(l, "\\\\z", __LINE__, 15, TOK_RE_EOF);
  _lex_add(l, "\\|", __LINE__, 15, TOK_RE_ALT);
  _lex_add(l, "\\(", __LINE__, 15, TOK_RE_LPAREN);
  _lex_add(l, "\\)", __LINE__, 15, TOK_RE_RPAREN);
  _lex_add(l, "\\?", __LINE__, 15, TOK_RE_MAYBE);
  _lex_add(l, "\\+", __LINE__, 15, TOK_RE_PLUS);
  _lex_add(l, "\\*", __LINE__, 15, TOK_RE_STAR);

  _lex_add(l, "\\\\u\\{[0-9a-fA-F]+\\}", __LINE__, 15, TOK_CODEPOINT);
  _lex_add(l, "\\\\[bfnrtv]", __LINE__, 15, TOK_C_ESCAPE);
  _lex_add(l, "\\\\.", __LINE__, 15, TOK_PLAIN_ESCAPE);
  _lex_add(l, ".", __LINE__, 15, TOK_CHAR);

  return l;
}

static Lex* _build_charclass_scope(void) {
  Lex* l = _lex_new("lex_charclass", "nest", "");

  _lex_add(l, "\\]", __LINE__, 15, TOK_CLASS_END);
  _lex_add(l, ".-", __LINE__, 15, TOK_RANGE_SEP);

  _lex_add(l, "\\\\u\\{[0-9a-fA-F]+\\}", __LINE__, 15, TOK_CODEPOINT);
  _lex_add(l, "\\\\[bfnrtv]", __LINE__, 15, TOK_C_ESCAPE);
  _lex_add(l, "\\\\.", __LINE__, 15, TOK_PLAIN_ESCAPE);
  _lex_add(l, ".", __LINE__, 15, TOK_CHAR);

  return l;
}

static Lex* _build_dquote_str_scope(void) {
  Lex* l = _lex_new("lex_dquote_str", "nest", "");

  _lex_add(l, "\"", __LINE__, 15, TOK_STR_END);
  _lex_add(l, "\\\\u\\{[0-9a-fA-F]+\\}", __LINE__, 15, TOK_CODEPOINT);
  _lex_add(l, "\\\\[bfnrtv]", __LINE__, 15, TOK_C_ESCAPE);
  _lex_add(l, "\\\\.", __LINE__, 15, TOK_PLAIN_ESCAPE);
  _lex_add(l, ".", __LINE__, 15, TOK_CHAR);

  return l;
}

static Lex* _build_squote_str_scope(void) {
  Lex* l = _lex_new("lex_squote_str", "nest", "");

  _lex_add(l, "'", __LINE__, 15, TOK_STR_END);
  _lex_add(l, "\\\\u\\{[0-9a-fA-F]+\\}", __LINE__, 15, TOK_CODEPOINT);
  _lex_add(l, "\\\\[bfnrtv]", __LINE__, 15, TOK_C_ESCAPE);
  _lex_add(l, "\\\\.", __LINE__, 15, TOK_PLAIN_ESCAPE);
  _lex_add(l, ".", __LINE__, 15, TOK_CHAR);

  return l;
}

static Lex* _build_peg_scope(void) {
  Lex* l = _lex_new("lex_peg", "nest", "");

  _lex_add(l, "[a-z_][a-zA-Z0-9_]*", __LINE__, 15, TOK_PEG_ID);
  _lex_add(l, "@[a-z_][a-zA-Z0-9_]*", __LINE__, 15, TOK_PEG_TOK_ID);
  _lex_add(l, ": *[a-z_][a-zA-Z0-9_]*", __LINE__, 15, TOK_TAG_ID);
  _lex_add(l, "=", __LINE__, 15, TOK_PEG_ASSIGN);
  _lex_add(l, "\\[", __LINE__, 15, TOK_BRANCHES_BEGIN);
  _lex_add(l, "\\]", __LINE__, 15, TOK_BRANCHES_END);

  _lex_add(l, "<", __LINE__, 15, TOK_PEG_LT);
  _lex_add(l, ">", __LINE__, 15, TOK_PEG_GT);
  _lex_add(l, "\\?", __LINE__, 15, TOK_PEG_QUESTION);
  _lex_add(l, "\\+", __LINE__, 15, TOK_PEG_PLUS);
  _lex_add(l, "\\*", __LINE__, 15, TOK_PEG_STAR);

  _lex_add(l, "\"", __LINE__, 15, TOK_STR_BEGIN);
  _lex_add(l, "'", __LINE__, 15, TOK_STR_BEGIN);

  _build_ignores(l);
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

  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "nest", ".");

  Lex* scopes[] = {
      _build_main_scope(),       _build_vpa_scope(),        _build_re_scope(),  _build_charclass_scope(),
      _build_dquote_str_scope(), _build_squote_str_scope(), _build_peg_scope(),
  };
  int32_t nscopes = sizeof(scopes) / sizeof(scopes[0]);

  for (int32_t i = 0; i < nscopes; i++) {
    _lex_gen_func(scopes[i], w, false);
    _lex_del(scopes[i]);
  }

  irwriter_end(w);
  irwriter_del(w);
  fclose(f);

  printf("generated %s\n", output);
  return 0;
}
