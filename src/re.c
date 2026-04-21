#include "re.h"
#include "darray.h"
#include "xmalloc.h"

#include <assert.h>
#include <string.h>

#define MAX_UNICODE 0x10FFFF

ReRange* re_range_new(void) { return XCALLOC(1, sizeof(ReRange)); }

void re_range_del(ReRange* range) {
  if (!range) {
    return;
  }
  darray_del(range->ivs);
  XFREE(range);
}

void re_range_add(ReRange* range, int32_t start_cp, int32_t end_cp) {
  assert(start_cp <= end_cp);
  assert(start_cp >= 0 && end_cp <= MAX_UNICODE);

  if (!range->ivs) {
    range->ivs = darray_new(sizeof(ReInterval), 0);
  }

  int32_t len = (int32_t)darray_size(range->ivs);
  int32_t lo = 0, hi = len;
  while (lo < hi) {
    int32_t mid = lo + (hi - lo) / 2;
    if (range->ivs[mid].end < start_cp - 1) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  int32_t first = lo;
  int32_t last = first;
  while (last < len && range->ivs[last].start <= end_cp + 1) {
    last++;
  }

  if (first == last) {
    range->ivs = darray_grow(range->ivs, (size_t)(len + 1));
    memmove(&range->ivs[first + 1], &range->ivs[first], (size_t)(len - first) * sizeof(ReInterval));
    range->ivs[first] = (ReInterval){start_cp, end_cp};
  } else {
    int32_t merged_start = start_cp < range->ivs[first].start ? start_cp : range->ivs[first].start;
    int32_t merged_end = end_cp > range->ivs[last - 1].end ? end_cp : range->ivs[last - 1].end;
    range->ivs[first] = (ReInterval){merged_start, merged_end};
    int32_t removed = last - first - 1;
    if (removed > 0) {
      memmove(&range->ivs[first + 1], &range->ivs[last], (size_t)(len - last) * sizeof(ReInterval));
      range->ivs = darray_grow(range->ivs, (size_t)(len - removed));
    }
  }
}

void re_range_neg(ReRange* range) {
  ReInterval* gaps = darray_new(sizeof(ReInterval), 0);
  int32_t pos = 0;

  for (int32_t i = 0; i < (int32_t)darray_size(range->ivs); i++) {
    if (pos < range->ivs[i].start) {
      ReInterval iv = {pos, range->ivs[i].start - 1};
      darray_push(gaps, iv);
    }
    pos = range->ivs[i].end + 1;
  }
  if (pos <= MAX_UNICODE) {
    ReInterval iv = {pos, MAX_UNICODE};
    darray_push(gaps, iv);
  }

  darray_del(range->ivs);
  range->ivs = gaps;
  range->negated = !range->negated;
}

void re_range_ic(ReRange* range) {
  bool was_neg = range->negated;
  if (was_neg) {
    re_range_neg(range);
  }

  int32_t n = (int32_t)darray_size(range->ivs);
  for (int32_t i = 0; i < n; i++) {
    int32_t lo = range->ivs[i].start;
    int32_t hi = range->ivs[i].end;
    // if overlaps a-z, add A-Z counterpart
    if (lo <= 'z' && hi >= 'a') {
      int32_t clo = (lo > 'a' ? lo : 'a') - 32;
      int32_t chi = (hi < 'z' ? hi : 'z') - 32;
      re_range_add(range, clo, chi);
    }
    // if overlaps A-Z, add a-z counterpart
    if (lo <= 'Z' && hi >= 'A') {
      int32_t clo = (lo > 'A' ? lo : 'A') + 32;
      int32_t chi = (hi < 'Z' ? hi : 'Z') + 32;
      re_range_add(range, clo, chi);
    }
  }

  if (was_neg) {
    re_range_neg(range);
  }
}

typedef struct {
  int32_t start_state;
  int32_t cur_state;
  int32_t* branch_ends;
} GroupFrame;

struct Re {
  Aut* aut;
  int32_t next_state;
  GroupFrame* stack;
};

static int32_t _alloc_state(Re* re) { return re->next_state++; }

static GroupFrame* _top(Re* re) {
  size_t sz = darray_size(re->stack);
  assert(sz > 0);
  return &re->stack[sz - 1];
}

static void _push_frame(Re* re, int32_t start, int32_t cur) {
  GroupFrame frame = {.start_state = start, .cur_state = cur, .branch_ends = NULL};
  darray_push(re->stack, frame);
}

static void _save_branch_end(GroupFrame* f, int32_t state) {
  if (!f->branch_ends) {
    f->branch_ends = darray_new(sizeof(int32_t), 0);
  }
  darray_push(f->branch_ends, state);
}

Re* re_new(Aut* aut) {
  Re* re = XCALLOC(1, sizeof(Re));
  re->aut = aut;
  re->next_state = 0;
  re->stack = darray_new(sizeof(GroupFrame), 0);
  _alloc_state(re);
  _push_frame(re, 0, 0);
  return re;
}

void re_del(Re* re) {
  if (!re) {
    return;
  }
  size_t sz = darray_size(re->stack);
  for (size_t i = 0; i < sz; i++) {
    darray_del(re->stack[i].branch_ends);
  }
  darray_del(re->stack);
  XFREE(re);
}

void re_append_ch(Re* re, int32_t codepoint, DebugInfo di) {
  GroupFrame* f = _top(re);
  int32_t s = _alloc_state(re);
  aut_transition(re->aut, (TransitionDef){f->cur_state, s, codepoint, codepoint}, di);
  f->cur_state = s;
}

void re_append_ch_ic(Re* re, int32_t codepoint, DebugInfo di) {
  GroupFrame* f = _top(re);
  int32_t s = _alloc_state(re);
  aut_transition(re->aut, (TransitionDef){f->cur_state, s, codepoint, codepoint}, di);
  if (codepoint >= 'a' && codepoint <= 'z') {
    aut_transition(re->aut, (TransitionDef){f->cur_state, s, codepoint - 32, codepoint - 32}, di);
  } else if (codepoint >= 'A' && codepoint <= 'Z') {
    aut_transition(re->aut, (TransitionDef){f->cur_state, s, codepoint + 32, codepoint + 32}, di);
  }
  f->cur_state = s;
}

void re_append_range(Re* re, ReRange* range, DebugInfo di) {
  assert(darray_size(range->ivs) > 0);
  GroupFrame* f = _top(re);
  int32_t s = _alloc_state(re);
  for (int32_t i = 0; i < (int32_t)darray_size(range->ivs); i++) {
    aut_transition(re->aut, (TransitionDef){f->cur_state, s, range->ivs[i].start, range->ivs[i].end}, di);
  }
  f->cur_state = s;
}

void re_append_group_s(ReRange* range) {
  re_range_add(range, '\t', '\r');
  re_range_add(range, ' ', ' ');
}

void re_append_group_d(ReRange* range) { re_range_add(range, '0', '9'); }

void re_append_group_w(ReRange* range) {
  re_range_add(range, '0', '9');
  re_range_add(range, 'A', 'Z');
  re_range_add(range, '_', '_');
  re_range_add(range, 'a', 'z');
}

void re_append_group_h(ReRange* range) {
  re_range_add(range, '0', '9');
  re_range_add(range, 'A', 'F');
  re_range_add(range, 'a', 'f');
}

void re_append_group_dot(ReRange* range) {
  re_range_add(range, 0, 9);
  re_range_add(range, 11, MAX_UNICODE);
}

int32_t re_c_escape(char symbol) {
  switch (symbol) {
  case 'b':
    return '\b';
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

int32_t re_hex_to_codepoint(const char* h, size_t size) {
  int32_t cp = 0;
  for (size_t i = 0; i < size; i++) {
    cp <<= 4;
    char c = h[i];
    if (c >= '0' && c <= '9') {
      cp |= c - '0';
    } else if (c >= 'a' && c <= 'f') {
      cp |= c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      cp |= c - 'A' + 10;
    }
  }
  return cp;
}

void re_lparen(Re* re) {
  GroupFrame* f = _top(re);
  int32_t start = f->cur_state;
  int32_t branch = _alloc_state(re);
  aut_epsilon(re->aut, start, branch);
  _push_frame(re, start, branch);
}

void re_fork(Re* re) {
  GroupFrame* f = _top(re);
  _save_branch_end(f, f->cur_state);
  int32_t branch = _alloc_state(re);
  aut_epsilon(re->aut, f->start_state, branch);
  f->cur_state = branch;
}

void re_rparen(Re* re) {
  size_t sz = darray_size(re->stack);
  assert(sz > 1);
  GroupFrame* f = &re->stack[sz - 1];
  _save_branch_end(f, f->cur_state);

  int32_t exit_state = _alloc_state(re);
  for (int32_t i = 0; i < (int32_t)darray_size(f->branch_ends); i++) {
    aut_epsilon(re->aut, f->branch_ends[i], exit_state);
  }

  darray_del(f->branch_ends);
  re->stack = darray_grow(re->stack, sz - 1);

  GroupFrame* parent = _top(re);
  parent->cur_state = exit_state;
}

void re_action(Re* re, int32_t action_id) {
  GroupFrame* f = _top(re);
  int32_t s = _alloc_state(re);
  aut_epsilon(re->aut, f->cur_state, s);
  aut_action(re->aut, s, action_id);
  f->cur_state = s;
}

void re_loop_back(Re* re) {
  GroupFrame* f = _top(re);
  aut_epsilon(re->aut, f->cur_state, f->start_state);
}

int32_t re_cur_state(Re* re) {
  GroupFrame* f = _top(re);
  return f->cur_state;
}

// --- ReLex: regex pattern → NFA compiler ---

#include "ustr.h"
#include <stdio.h>

#define RELEX_ERR_PAREN (-1)
#define RELEX_ERR_BRACKET (-2)

struct ReLex {
  Aut* aut;
  Re* re;
  const char* source_file;
  bool started;
  bool icase;
  bool binary;
};

typedef struct {
  ReLex* lex;
  int32_t* cps;
  int32_t ncp;
  int32_t pos;
  bool binary;
  bool icase;
  int32_t line;
  int32_t col;
  char* ustr_buf;
  int32_t err;
} ReLexParser;

static void _rlp_parse_expr(ReLexParser* p);

static int32_t _rlp_peek(ReLexParser* p) {
  if (p->pos >= p->ncp) {
    return -1;
  }
  return p->cps[p->pos];
}

static int32_t _rlp_advance(ReLexParser* p) {
  if (p->pos >= p->ncp) {
    return -1;
  }
  return p->cps[p->pos++];
}

static DebugInfo _rlp_di(ReLexParser* p) { return (DebugInfo){p->line, p->col + p->pos}; }

static void _rlp_range_add_icase(ReRange* range, int32_t start, int32_t end, bool icase) {
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

static void _rlp_emit_ch(ReLexParser* p, int32_t cp) {
  Re* re = p->lex->re;
  DebugInfo di = _rlp_di(p);
  if (p->icase && ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'))) {
    ReRange* range = re_range_new();
    _rlp_range_add_icase(range, cp, cp, true);
    re_append_range(re, range, di);
    re_range_del(range);
  } else {
    re_append_ch(re, cp, di);
  }
}

static void _rlp_add_class_ranges(ReRange* range, int32_t cls) {
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

static int32_t _rlp_parse_unicode_escape(ReLexParser* p) {
  _rlp_advance(p);
  int32_t cp = 0;
  while (_rlp_peek(p) != '}' && _rlp_peek(p) >= 0) {
    int32_t ch = _rlp_advance(p);
    if (ch >= '0' && ch <= '9') {
      cp = cp * 16 + (ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      cp = cp * 16 + (ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      cp = cp * 16 + (ch - 'A' + 10);
    } else {
      fprintf(stderr, "re_lex: invalid hex char '%c' in \\u{...} escape\n", (char)ch);
      exit(1);
    }
  }
  if (_rlp_peek(p) == '}') {
    _rlp_advance(p);
  }
  return cp;
}

static int32_t _rlp_parse_class_escape(ReLexParser* p, ReRange* range) {
  int32_t ch = _rlp_advance(p);
  if (ch < 0) {
    return -1;
  }
  int32_t esc = re_c_escape((char)ch);
  if (esc >= 0) {
    return esc;
  }
  switch (ch) {
  case 'u':
    return _rlp_parse_unicode_escape(p);
  case 's':
  case 'w':
  case 'd':
  case 'h':
    _rlp_add_class_ranges(range, ch);
    return -1;
  default:
    return ch;
  }
}

static void _rlp_parse_charclass(ReLexParser* p) {
  if (p->err) {
    return;
  }
  _rlp_advance(p);

  bool neg = false;
  if (_rlp_peek(p) == '^') {
    _rlp_advance(p);
    neg = true;
  }

  ReRange* range = re_range_new();

  while (_rlp_peek(p) != ']' && _rlp_peek(p) >= 0 && !p->err) {
    int32_t cp;
    if (_rlp_peek(p) == '\\') {
      _rlp_advance(p);
      cp = _rlp_parse_class_escape(p, range);
    } else {
      cp = _rlp_advance(p);
    }

    if (cp >= 0 && _rlp_peek(p) == '-' && p->pos + 1 < p->ncp && p->cps[p->pos + 1] != ']') {
      _rlp_advance(p);
      int32_t cp2;
      if (_rlp_peek(p) == '\\') {
        _rlp_advance(p);
        cp2 = _rlp_parse_class_escape(p, range);
      } else {
        cp2 = _rlp_advance(p);
      }
      if (cp2 >= 0) {
        _rlp_range_add_icase(range, cp, cp2, p->icase);
      } else {
        _rlp_range_add_icase(range, cp, cp, p->icase);
      }
    } else if (cp >= 0) {
      _rlp_range_add_icase(range, cp, cp, p->icase);
    }
  }

  if (_rlp_peek(p) != ']') {
    p->err = RELEX_ERR_BRACKET;
    re_range_del(range);
    return;
  }
  _rlp_advance(p);

  if (neg) {
    re_range_neg(range);
  }

  re_append_range(p->lex->re, range, _rlp_di(p));
  re_range_del(range);
}

static void _rlp_parse_atom(ReLexParser* p) {
  if (p->err) {
    return;
  }

  int32_t ch = _rlp_peek(p);

  if (ch == '(') {
    _rlp_advance(p);
    re_lparen(p->lex->re);
    _rlp_parse_expr(p);
    if (p->err) {
      return;
    }
    if (_rlp_peek(p) != ')') {
      p->err = RELEX_ERR_PAREN;
      return;
    }
    _rlp_advance(p);
    re_rparen(p->lex->re);
  } else if (ch == '[') {
    _rlp_parse_charclass(p);
  } else if (ch == '.') {
    _rlp_advance(p);
    ReRange* range = re_range_new();
    if (p->binary) {
      re_range_add(range, 0, 9);
      re_range_add(range, 11, 255);
    } else {
      re_range_add(range, 0, 9);
      re_range_add(range, 11, MAX_UNICODE);
    }
    re_append_range(p->lex->re, range, _rlp_di(p));
    re_range_del(range);
  } else if (ch == '\\') {
    _rlp_advance(p);
    ch = _rlp_peek(p);
    if (ch < 0) {
      return;
    }
    if (ch == 's' || ch == 'w' || ch == 'd' || ch == 'h') {
      _rlp_advance(p);
      ReRange* range = re_range_new();
      _rlp_add_class_ranges(range, ch);
      re_append_range(p->lex->re, range, _rlp_di(p));
      re_range_del(range);
    } else if (ch == 'u') {
      _rlp_advance(p);
      int32_t cp = _rlp_parse_unicode_escape(p);
      _rlp_emit_ch(p, cp);
    } else {
      int32_t esc = re_c_escape((char)ch);
      _rlp_advance(p);
      _rlp_emit_ch(p, esc >= 0 ? esc : ch);
    }
  } else {
    _rlp_advance(p);
    _rlp_emit_ch(p, ch);
  }
}

static void _rlp_parse_quantified(ReLexParser* p) {
  if (p->err) {
    return;
  }

  Re* re = p->lex->re;
  Aut* aut = p->lex->aut;
  int32_t before = re_cur_state(re);

  _rlp_parse_atom(p);
  if (p->err) {
    return;
  }

  int32_t ch = _rlp_peek(p);
  if (ch == '?') {
    _rlp_advance(p);
    aut_epsilon(aut, before, re_cur_state(re));
  } else if (ch == '+') {
    _rlp_advance(p);
    aut_epsilon(aut, re_cur_state(re), before);
  } else if (ch == '*') {
    _rlp_advance(p);
    aut_epsilon(aut, before, re_cur_state(re));
    aut_epsilon(aut, re_cur_state(re), before);
  }
}

static void _rlp_parse_branch(ReLexParser* p) {
  while (!p->err) {
    int32_t ch = _rlp_peek(p);
    if (ch < 0 || ch == '|' || ch == ')') {
      break;
    }
    _rlp_parse_quantified(p);
  }
}

static void _rlp_parse_expr(ReLexParser* p) {
  if (p->err) {
    return;
  }
  _rlp_parse_branch(p);
  while (_rlp_peek(p) == '|' && !p->err) {
    _rlp_advance(p);
    re_fork(p->lex->re);
    _rlp_parse_branch(p);
  }
}

ReLex* re_lex_new(const char* func_name, const char* source_file_name, const char* mode) {
  ReLex* l = XCALLOC(1, sizeof(ReLex));
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

void re_lex_del(ReLex* l) {
  if (!l) {
    return;
  }
  re_del(l->re);
  aut_del(l->aut);
  XFREE(l);
}

int32_t re_lex_add(ReLex* l, const char* pattern, int32_t line, int32_t col, int32_t action_id) {
  if (l->started) {
    re_fork(l->re);
  }
  l->started = true;

  ReLexParser p = {0};
  p.lex = l;
  p.line = line;
  p.col = col;
  p.icase = l->icase;
  p.binary = l->binary;

  int32_t len = (int32_t)strlen(pattern);
  if (p.binary) {
    p.ncp = len;
    p.cps = XMALLOC((size_t)len * sizeof(int32_t));
    for (int32_t i = 0; i < len; i++) {
      p.cps[i] = (uint8_t)pattern[i];
    }
  } else {
    p.ustr_buf = ustr_new((size_t)len, pattern);
    p.ncp = ustr_size(p.ustr_buf);
    if (p.ncp > 0) {
      p.cps = XMALLOC((size_t)p.ncp * sizeof(int32_t));
      UstrIter it;
      ustr_iter_init(&it, p.ustr_buf, 0);
      for (int32_t i = 0; i < p.ncp; i++) {
        p.cps[i] = ustr_iter_next(&it);
      }
    }
  }

  _rlp_parse_expr(&p);

  int32_t err = p.err;
  XFREE(p.cps);
  if (p.ustr_buf) {
    ustr_del(p.ustr_buf);
  }

  if (err) {
    return err;
  }

  re_action(l->re, action_id);
  return action_id;
}

void re_lex_gen(ReLex* l, IrWriter* w, bool debug_mode) {
  re_rparen(l->re);
  aut_optimize(l->aut);
  aut_gen_dfa(l->aut, w, debug_mode);
}
