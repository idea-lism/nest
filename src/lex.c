#include "lex.h"
#include "re.h"
#include "ustr.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct Lex {
  Aut* aut;
  Re* re;
  const char* source_file;
  int32_t next_action;
  bool started;
};

typedef struct {
  Lex* lex;
  int32_t* cps;
  int32_t ncp;
  int32_t pos;
  bool binary;
  bool icase;
  int32_t offset;
  char* ustr_buf;
  int32_t err;
} Parser;

static void _parse_expr(Parser* p);

// --- Parser helpers ---

static int32_t _peek(Parser* p) {
  if (p->pos >= p->ncp) {
    return -1;
  }
  return p->cps[p->pos];
}

static int32_t _advance(Parser* p) {
  if (p->pos >= p->ncp) {
    return -1;
  }
  return p->cps[p->pos++];
}

static DebugInfo _di(Parser* p) { return (DebugInfo){p->offset, p->pos}; }

// --- Case helpers ---

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

static void _emit_ch(Parser* p, int32_t cp) {
  Re* re = p->lex->re;
  DebugInfo di = _di(p);
  if (p->icase && ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'))) {
    ReRange* range = re_range_new();
    _range_add_icase(range, cp, cp, true);
    re_append_range(re, range, di);
    re_range_del(range);
  } else {
    re_append_ch(re, cp, di);
  }
}

// --- Special character classes ---

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

// --- Escape parsing ---

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

static int32_t _parse_unicode_escape(Parser* p) {
  _advance(p); // consume '{'
  int32_t cp = 0;
  while (_peek(p) != '}' && _peek(p) >= 0) {
    int32_t ch = _advance(p);
    if (ch >= '0' && ch <= '9') {
      cp = cp * 16 + (ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      cp = cp * 16 + (ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      cp = cp * 16 + (ch - 'A' + 10);
    }
  }
  if (_peek(p) == '}') {
    _advance(p);
  }
  return cp;
}

// Returns codepoint, or -1 if special class was added to range directly.
static int32_t _parse_class_escape(Parser* p, ReRange* range) {
  int32_t ch = _advance(p);
  if (ch < 0) {
    return -1;
  }
  int32_t esc = _c_escape(ch);
  if (esc >= 0) {
    return esc;
  }
  switch (ch) {
  case 'u':
    return _parse_unicode_escape(p);
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

// --- Parse functions ---

static void _parse_charclass(Parser* p) {
  if (p->err) {
    return;
  }
  _advance(p); // consume '['

  bool neg = false;
  if (_peek(p) == '^') {
    _advance(p);
    neg = true;
  }

  ReRange* range = re_range_new();

  while (_peek(p) != ']' && _peek(p) >= 0 && !p->err) {
    int32_t cp;
    if (_peek(p) == '\\') {
      _advance(p);
      cp = _parse_class_escape(p, range);
    } else {
      cp = _advance(p);
    }

    if (cp >= 0 && _peek(p) == '-' && p->pos + 1 < p->ncp && p->cps[p->pos + 1] != ']') {
      _advance(p); // consume '-'
      int32_t cp2;
      if (_peek(p) == '\\') {
        _advance(p);
        cp2 = _parse_class_escape(p, range);
      } else {
        cp2 = _advance(p);
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

  if (_peek(p) != ']') {
    p->err = LEX_ERR_BRACKET;
    re_range_del(range);
    return;
  }
  _advance(p); // consume ']'

  if (neg) {
    re_range_neg(range);
  }

  re_append_range(p->lex->re, range, _di(p));
  re_range_del(range);
}

static void _parse_atom(Parser* p) {
  if (p->err) {
    return;
  }

  int32_t ch = _peek(p);

  if (ch == '(') {
    _advance(p);
    re_lparen(p->lex->re);
    _parse_expr(p);
    if (p->err) {
      return;
    }
    if (_peek(p) != ')') {
      p->err = LEX_ERR_PAREN;
      return;
    }
    _advance(p);
    re_rparen(p->lex->re);
  } else if (ch == '[') {
    _parse_charclass(p);
  } else if (ch == '.') {
    _advance(p);
    ReRange* range = re_range_new();
    if (p->binary) {
      re_range_add(range, 0, 9);
      re_range_add(range, 11, 255);
    } else {
      re_range_add(range, 0, 9);
      re_range_add(range, 11, 0x10FFFF);
    }
    re_append_range(p->lex->re, range, _di(p));
    re_range_del(range);
  } else if (ch == '\\') {
    _advance(p);
    ch = _peek(p);
    if (ch < 0) {
      return;
    }
    if (ch == 'a') {
      _advance(p);
      re_append_ch(p->lex->re, LEX_CP_BOF, _di(p));
    } else if (ch == 'z') {
      _advance(p);
      re_append_ch(p->lex->re, LEX_CP_EOF, _di(p));
    } else if (ch == 's' || ch == 'w' || ch == 'd' || ch == 'h') {
      _advance(p);
      ReRange* range = re_range_new();
      _add_class_ranges(range, ch);
      re_append_range(p->lex->re, range, _di(p));
      re_range_del(range);
    } else if (ch == 'u') {
      _advance(p);
      int32_t cp = _parse_unicode_escape(p);
      _emit_ch(p, cp);
    } else {
      int32_t esc = _c_escape(ch);
      _advance(p);
      _emit_ch(p, esc >= 0 ? esc : ch);
    }
  } else {
    _advance(p);
    _emit_ch(p, ch);
  }
}

static void _parse_quantified(Parser* p) {
  if (p->err) {
    return;
  }

  Re* re = p->lex->re;
  Aut* aut = p->lex->aut;
  int32_t before = re_cur_state(re);

  _parse_atom(p);
  if (p->err) {
    return;
  }

  int32_t ch = _peek(p);
  if (ch == '?') {
    _advance(p);
    aut_epsilon(aut, before, re_cur_state(re));
  } else if (ch == '+') {
    _advance(p);
    aut_epsilon(aut, re_cur_state(re), before);
  } else if (ch == '*') {
    _advance(p);
    aut_epsilon(aut, before, re_cur_state(re));
    aut_epsilon(aut, re_cur_state(re), before);
  }
}

static void _parse_branch(Parser* p) {
  while (!p->err) {
    int32_t ch = _peek(p);
    if (ch < 0 || ch == '|' || ch == ')') {
      break;
    }
    _parse_quantified(p);
  }
}

static void _parse_expr(Parser* p) {
  if (p->err) {
    return;
  }
  _parse_branch(p);
  while (_peek(p) == '|' && !p->err) {
    _advance(p);
    re_fork(p->lex->re);
    _parse_branch(p);
  }
}

// --- Public API ---

Lex* lex_new(const char* source_file_name) {
  Lex* l = calloc(1, sizeof(Lex));
  l->aut = aut_new("lex", source_file_name);
  l->re = re_new(l->aut);
  l->source_file = source_file_name;
  l->next_action = 1;
  re_lparen(l->re);
  return l;
}

void lex_del(Lex* l) {
  if (!l) {
    return;
  }
  re_del(l->re);
  aut_del(l->aut);
  free(l);
}

int32_t lex_add(Lex* l, const char* mode, const char* pattern, int32_t source_file_offset) {
  if (l->started) {
    re_fork(l->re);
  }
  l->started = true;

  Parser p = {0};
  p.lex = l;
  p.offset = source_file_offset;

  for (const char* m = mode; *m; m++) {
    if (*m == 'i') {
      p.icase = true;
    }
    if (*m == 'b') {
      p.binary = true;
    }
  }

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

  _parse_expr(&p);

  int32_t err = p.err;
  free(p.cps);
  if (p.ustr_buf) {
    ustr_del(p.ustr_buf);
  }

  if (err) {
    return err;
  }

  int32_t action_id = l->next_action++;
  re_action(l->re, action_id);
  return action_id;
}

void lex_gen(Lex* l, FILE* f, const char* target_triple) {
  re_rparen(l->re);
  aut_optimize(l->aut);

  IrWriter* w = irwriter_new(f, target_triple);
  irwriter_start(w, l->source_file, ".");
  aut_gen_dfa(l->aut, w, false);
  irwriter_end(w);
  irwriter_del(w);
}
