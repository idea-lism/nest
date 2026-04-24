// specs/parse.md — structure matches 1:1 to specs/bootstrap.nest PEG
#include "parse.h"
#include "darray.h"
#include "re.h"
#include "re_ir.h"
#include "ustr.h"
#include "xmalloc.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  int64_t state;
  int64_t action;
} LexResult;

struct SharedState {
  int32_t last_quote_cp;
  bool re_mode_icase;
  bool re_mode_binary;
  bool cc_kind_neg;
};

extern LexResult lex_main(int64_t, int64_t);
extern LexResult lex_vpa(int64_t, int64_t);
extern LexResult lex_scope(int64_t, int64_t);
extern LexResult lex_lit_scope(int64_t, int64_t);
extern LexResult lex_peg(int64_t, int64_t);
extern LexResult lex_branches(int64_t, int64_t);
extern LexResult lex_peg_tag(int64_t, int64_t);
extern LexResult lex_re(int64_t, int64_t);
extern LexResult lex_re_ref(int64_t, int64_t);
extern LexResult lex_charclass(int64_t, int64_t);
extern LexResult lex_re_str(int64_t, int64_t);
extern LexResult lex_peg_str(int64_t, int64_t);

#define LEX_ACTION_NOMATCH (-2)

typedef LexResult (*LexFunc)(int64_t, int64_t);

// ============================================================================
// Error reporting
// ============================================================================

bool parse_has_error(ParseState* ps) { return ps->error[0] != '\0'; }

static void _error_at(ParseState* ps, Token* t, const char* fmt, ...) {
  if (parse_has_error(ps)) {
    return;
  }
  int32_t off = 0;
  if (t && ps->tree && t->term_id) {
    Location loc = tt_locate(ps->tree, t->cp_start);
    off = snprintf(ps->error, sizeof(ps->error), "%d:%d: ", loc.line, loc.col);
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ps->error + off, sizeof(ps->error) - (size_t)off, fmt, ap);
  va_end(ap);
}

void parse_error(ParseState* ps, const char* fmt, ...) {
  if (parse_has_error(ps)) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ps->error, sizeof(ps->error), fmt, ap);
  va_end(ap);
}

// ============================================================================
// Token cursor — sentinel based (tokens[size].term_id == 0)
// ============================================================================

// current token (safe: sentinel has term_id=0)
#define T(p) (chunk->tokens[*(p)])

static bool _eat(TokenChunk* chunk, int32_t* p, int32_t id) {
  if (T(p).term_id == id) {
    (*p)++;
    return true;
  }
  return false;
}

static void _skip_nl(TokenChunk* chunk, int32_t* p) {
  while (T(p).term_id == TOK_NL) {
    (*p)++;
  }
}

static Location _loc(ParseState* ps, Token* t) { return tt_locate(ps->tree, t->cp_start); }
static TokenChunk* _chunk(ParseState* ps, Token* t) { return &ps->tree->table[t->chunk_id]; }

// take chunk->value, NULL it (ownership transfer)
static void* _take(TokenChunk* c) {
  void* v = c->value;
  c->value = NULL;
  return v;
}

static char* _take_aux(TokenChunk* c) {
  char* v = (char*)c->aux_value;
  c->aux_value = NULL;
  return v;
}

// ============================================================================
// String / intern helpers
// ============================================================================

static char* _cp_strdup(const char* src, int32_t cp_start, int32_t cp_size) {
  UstrIter it = {0};
  ustr_iter_init(&it, src, cp_start);
  int32_t start_byte = it.byte_index;
  for (int32_t i = 0; i < cp_size; i++) {
    ustr_iter_next(&it);
  }
  int32_t byte_len = it.byte_index - start_byte;
  char* s = XMALLOC((size_t)byte_len + 1);
  memcpy(s, src + start_byte, (size_t)byte_len);
  s[byte_len] = '\0';
  return s;
}

static char* _tok_str(ParseState* ps, Token* t) { return _cp_strdup(ps->src, t->cp_start, t->cp_size); }

static int32_t _intern_tok(Symtab* st, const char* src, Token* t) {
  UstrIter it = {0};
  ustr_iter_init(&it, src, t->cp_start);
  int32_t start_byte = it.byte_index;
  for (int32_t i = 0; i < t->cp_size; i++) {
    ustr_iter_next(&it);
  }
  return symtab_intern_f(st, "%.*s", it.byte_index - start_byte, src + start_byte);
}

__attribute__((format(printf, 1, 2))) char* parse_sfmt(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int32_t len = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char* s = XMALLOC((size_t)len + 1);
  va_start(ap, fmt);
  vsnprintf(s, (size_t)len + 1, fmt, ap);
  va_end(ap);
  return s;
}

void parse_set_str(char** dst, char* s) {
  XFREE(*dst);
  *dst = s;
}

static int32_t _decode_cp(const char* src, Token* t) {
  UstrIter it = {0};
  ustr_iter_init(&it, src, t->cp_start);
  const char* p = src + it.byte_index;
  if (t->term_id == TOK_CODEPOINT) {
    return re_hex_to_codepoint(p + 2, (size_t)(t->cp_size - 3));
  }
  if (t->term_id == TOK_C_ESCAPE && t->cp_size >= 2) {
    int32_t esc = re_c_escape(p[1]);
    return esc >= 0 ? esc : (int32_t)(unsigned char)p[1];
  }
  if (t->term_id == TOK_PLAIN_ESCAPE && t->cp_size >= 2) {
    ustr_iter_next(&it);
    return ustr_iter_next(&it);
  }
  return ustr_iter_next(&it);
}

static bool _is_str_char(int32_t id) {
  return id == TOK_CHAR || id == TOK_CODEPOINT || id == TOK_C_ESCAPE || id == TOK_PLAIN_ESCAPE;
}

// fragment lookup/create
static int32_t _frag_id(ParseState* ps, Token* t) { return _intern_tok(&ps->re_frag_names, ps->src, t); }

// ============================================================================
// Pushdown automaton lexer
// ============================================================================

typedef bool (*ParseFunc)(ParseState*, TokenChunk*);

typedef struct {
  const char* scope_name;
  LexFunc lex_fn;
  ParseFunc parse_fn;
  bool eof_end;
} ScopeConfig;

typedef struct {
  ParseState* ps;
  TokenTree* tree;
  int32_t cp_count;
  UstrIter it;
  SharedState shared;
} LexCtx;

static int32_t _next_cp(LexCtx* ctx) {
  if (ctx->it.cp_index >= ctx->cp_count) {
    return -2;
  }
  int32_t idx = ctx->it.cp_index;
  int32_t cp = ustr_iter_next(&ctx->it);
  if (cp == '\n') {
    ctx->tree->newline_map[idx / 64] |= (1ULL << (idx % 64));
  }
  return cp;
}

// forward-declare parse_fn functions
static bool _parse_re(ParseState* ps, TokenChunk* chunk);
static bool _parse_charclass(ParseState* ps, TokenChunk* chunk); // charclass = charclass_unit+
static bool _parse_re_str(ParseState* ps, TokenChunk* chunk);
static bool _parse_peg_str(ParseState* ps, TokenChunk* chunk);
static bool _parse_vpa(ParseState* ps, TokenChunk* chunk);
static bool _parse_peg(ParseState* ps, TokenChunk* chunk);
static bool _parse_scope(ParseState* ps, TokenChunk* chunk);
static bool _parse_lit_scope(ParseState* ps, TokenChunk* chunk);
static bool _parse_branches(ParseState* ps, TokenChunk* chunk);

static bool _end_scope(LexCtx* ctx, ScopeId scope_id, ParseFunc parse_fn) {
  if (parse_fn) {
    TokenChunk* child = ctx->tree->current;
    child->scope_id = scope_id;
    tt_pop(ctx->tree, ctx->it.cp_index);
    return parse_fn(ctx->ps, child);
  }
  return true;
}

static bool _lex_scope(LexCtx* ctx, ScopeId scope_id) {
  static const ScopeConfig configs[] = {
      [SCOPE_MAIN] = {"main", lex_main, NULL, false},
      [SCOPE_VPA] = {"vpa", lex_vpa, _parse_vpa, false},
      [SCOPE_SCOPE] = {"scope", lex_scope, _parse_scope, false},
      [SCOPE_LIT_SCOPE] = {"lit", lex_lit_scope, _parse_lit_scope, false},
      [SCOPE_PEG] = {"peg", lex_peg, _parse_peg, true},
      [SCOPE_BRANCHES] = {"branches", lex_branches, _parse_branches, false},
      [SCOPE_PEG_TAG] = {"peg_tag", lex_peg_tag, NULL, false},
      [SCOPE_RE] = {"regexp", lex_re, _parse_re, false},
      [SCOPE_RE_REF] = {"re_ref", lex_re_ref, NULL, false},
      [SCOPE_CHARCLASS] = {"charclass", lex_charclass, _parse_charclass, false},
      [SCOPE_RE_STR] = {"re_str", lex_re_str, _parse_re_str, false},
      [SCOPE_PEG_STR] = {"peg_str", lex_peg_str, _parse_peg_str, false},
  };
  ScopeConfig cfg = configs[scope_id];

  int64_t state = 0;
  int32_t last_action = 0;
  int32_t tok_start = ctx->it.cp_index;

  for (;;) {
    if (ctx->it.cp_index >= ctx->cp_count && last_action == 0) {
      if (cfg.eof_end) {
        return _end_scope(ctx, scope_id, cfg.parse_fn);
      }
      break;
    }

    int32_t saved = ctx->it.cp_index;
    LexResult r = cfg.lex_fn(state, _next_cp(ctx));

    if (r.action != LEX_ACTION_NOMATCH) {
      state = r.state;
      last_action = (int32_t)r.action;
      continue;
    }
    ustr_iter_seek(&ctx->it, saved);

    if (last_action <= 0) {
      _error_at(ctx->ps, NULL, "unexpected character at position %d inside %s scope", saved, cfg.scope_name);
      return false;

    } else if (last_action < SCOPE_COUNT) {
      ScopeConfig child_cfg = configs[last_action];
      if (child_cfg.parse_fn) {
        tt_push(ctx->tree, last_action);
      }
      if (!_lex_scope(ctx, last_action)) {
        return false;
      }
      saved = ctx->it.cp_index;

    } else if (last_action < ACTION_COUNT) {
      switch (last_action) {
      case ACTION_IGNORE:
      case ACTION_BEGIN_NO_PUSH:
        break;
      case ACTION_BEGIN_PUSH:
        tt_push(ctx->tree, scope_id);
        break;
      case ACTION_END:
        return _end_scope(ctx, scope_id, cfg.parse_fn);
      case ACTION_UNPARSE:
        ustr_iter_seek(&ctx->it, tok_start);
        break;
      case ACTION_END_NL: {
        bool result = _end_scope(ctx, scope_id, cfg.parse_fn);
        tt_add(ctx->tree, TOK_NL, tok_start, saved - tok_start, -1);
        return result;
      }
      case ACTION_UNPARSE_END:
        ustr_iter_seek(&ctx->it, tok_start);
        return _end_scope(ctx, scope_id, cfg.parse_fn);
      case ACTION_STR_CHECK_END:
        if (ustr_cp_at(ctx->ps->src, tok_start) == ctx->shared.last_quote_cp) {
          return _end_scope(ctx, scope_id, cfg.parse_fn);
        }
        tt_add(ctx->tree, TOK_CHAR, tok_start, saved - tok_start, -1);
        break;
      case ACTION_SET_QUOTE_BEGIN: {
        ctx->shared.last_quote_cp = ustr_cp_at(ctx->ps->src, tok_start);
        int32_t child = scope_id == SCOPE_PEG || scope_id == SCOPE_BRANCHES ? SCOPE_PEG_STR : SCOPE_RE_STR;
        tt_push(ctx->tree, child);
        if (!_lex_scope(ctx, child)) {
          return false;
        }
        break;
      }
      case ACTION_SET_RE_MODE_BEGIN: {
        ctx->shared.re_mode_icase = false;
        ctx->shared.re_mode_binary = false;
        UstrIter mode_it = {0};
        ustr_iter_init(&mode_it, ctx->ps->src, tok_start);
        for (int32_t i = tok_start; i < saved; i++) {
          int32_t ch = ustr_iter_next(&mode_it);
          if (ch == '/') {
            break;
          }
          if (ch == 'i') {
            ctx->shared.re_mode_icase = true;
          }
          if (ch == 'b') {
            ctx->shared.re_mode_binary = true;
          }
        }
        tt_push(ctx->tree, SCOPE_RE);
        if (!_lex_scope(ctx, SCOPE_RE)) {
          return false;
        }
        break;
      }
      case ACTION_SET_CC_KIND_BEGIN: {
        ctx->shared.cc_kind_neg = false;
        UstrIter cc_it = {0};
        ustr_iter_init(&cc_it, ctx->ps->src, tok_start);
        for (int32_t i = tok_start; i < saved; i++) {
          if (ustr_iter_next(&cc_it) == '^') {
            ctx->shared.cc_kind_neg = true;
            break;
          }
        }
        tt_push(ctx->tree, SCOPE_CHARCLASS);
        if (!_lex_scope(ctx, SCOPE_CHARCLASS)) {
          return false;
        }
        break;
      }
      default:
        break;
      }
    } else {
      tt_add(ctx->tree, last_action, tok_start, saved - tok_start, -1);
    }

    tok_start = saved;
    last_action = 0;
    state = 0;
  } // end while

  if (scope_id == SCOPE_MAIN) {
    return true;
  }

  parse_error(ctx->ps, "unexpected end of input inside %s scope", cfg.scope_name);
  return false;
}

// ============================================================================
// RE recursive descent — specs/bootstrap.nest [[peg]] "Regex AST rules"
// ============================================================================

static ReIr _parse_re_expr(ParseState* ps, TokenChunk* chunk, int32_t* p, ReIr ir, bool icase);

// charclass_unit = [ charclass_char @range_sep charclass_char : range | charclass_char : single ]
static ReIr _parse_charclass_unit(ParseState* ps, TokenChunk* chunk, int32_t* p, ReIr ir) {
  Token* first = &T(p);
  Location loc = _loc(ps, first);
  int32_t lo = _decode_cp(ps->src, &T(p));
  (*p)++;
  if (_eat(chunk, p, TOK_RANGE_SEP)) {
    if (!_is_str_char(T(p).term_id)) {
      _error_at(ps, first, "incomplete range in character class");
      return ir;
    }
    int32_t hi = _decode_cp(ps->src, &T(p));
    (*p)++;
    return re_ir_emit(ir, RE_IR_APPEND_CH, lo, hi, loc.line, loc.col);
  }
  return re_ir_emit(ir, RE_IR_APPEND_CH, lo, lo, loc.line, loc.col);
}

// charclass = charclass_unit+
// charclass = charclass_unit+
static bool _parse_charclass(ParseState* ps, TokenChunk* chunk) {
  int32_t p = 0;
  bool neg = ps->shared->cc_kind_neg, icase = ps->shared->re_mode_icase;
  ReIr ir = re_ir_new();
  if (!_is_str_char(T(&p).term_id)) {
    _error_at(ps, &T(&p), "empty character class");
    chunk->value = ir;
    return false;
  }
  Location loc = _loc(ps, &T(&p));
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0, loc.line, loc.col);
  while (_is_str_char(T(&p).term_id)) {
    ir = _parse_charclass_unit(ps, chunk, &p, ir);
  }
  if (neg) {
    ir = re_ir_emit(ir, RE_IR_RANGE_NEG, 0, 0, loc.line, loc.col);
  }
  if (icase) {
    ir = re_ir_emit(ir, RE_IR_RANGE_IC, 0, 0, loc.line, loc.col);
  }
  chunk->value = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0, loc.line, loc.col);
  return true;
}

static ReIr _emit_shorthand(ReIr ir, int32_t tok_id, int32_t line, int32_t col) {
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0, line, col);
  switch (tok_id) {
  case TOK_RE_DOT:
    ir = re_ir_emit(ir, RE_IR_APPEND_CH, '\n', '\n', line, col);
    ir = re_ir_emit(ir, RE_IR_RANGE_NEG, 0, 0, line, col);
    break;
  case TOK_RE_SPACE_CLASS:
    ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_S, 0, 0, line, col);
    break;
  case TOK_RE_WORD_CLASS:
    ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_W, 0, 0, line, col);
    break;
  case TOK_RE_DIGIT_CLASS:
    ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_D, 0, 0, line, col);
    break;
  case TOK_RE_HEX_CLASS:
    ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_H, 0, 0, line, col);
    break;
  }
  return re_ir_emit(ir, RE_IR_RANGE_END, 0, 0, line, col);
}

// re_unit = [ "(" re ")" | charclass | @re_dot | ... | @re_ref | @char | ... ]
static ReIr _parse_re_unit(ParseState* ps, TokenChunk* chunk, int32_t* p, ReIr ir, bool icase) {
  Token* t = &T(p);
  Location loc = _loc(ps, t);
  switch (t->term_id) {
  case LIT_LPAREN:
    (*p)++;
    ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0, loc.line, loc.col);
    ir = _parse_re_expr(ps, chunk, p, ir, icase);
    if (T(p).term_id == LIT_RPAREN) {
      loc = _loc(ps, &T(p));
    }
    _eat(chunk, p, LIT_RPAREN);
    return re_ir_emit(ir, RE_IR_RPAREN, 0, 0, loc.line, loc.col);
  case SCOPE_CHARCLASS: {
    (*p)++;
    TokenChunk* cc = _chunk(ps, t);
    ReIr cc_ir = (ReIr)cc->value;
    return cc_ir ? darray_concat(ir, cc_ir) : ir;
  }
  case TOK_RE_DOT:
  case TOK_RE_SPACE_CLASS:
  case TOK_RE_WORD_CLASS:
  case TOK_RE_DIGIT_CLASS:
  case TOK_RE_HEX_CLASS:
    (*p)++;
    return _emit_shorthand(ir, t->term_id, loc.line, loc.col);
  case TOK_RE_REF: {
    (*p)++;
    return re_ir_emit(ir, RE_IR_FRAG_REF, _frag_id(ps, t), 0, loc.line, loc.col);
  }
  case TOK_CHAR:
  case TOK_CODEPOINT:
  case TOK_C_ESCAPE:
  case TOK_PLAIN_ESCAPE: {
    (*p)++;
    int32_t cp = _decode_cp(ps->src, t);
    return re_ir_emit(ir, icase ? RE_IR_APPEND_CH_IC : RE_IR_APPEND_CH, cp, cp, loc.line, loc.col);
  }
  default:
    return ir;
  }
}

// re_quantified = re_unit [ "?" | "+" | "*" | (none) ]
static ReIr _parse_re_quantified(ParseState* ps, TokenChunk* chunk, int32_t* p, ReIr ir, bool icase) {
  size_t s = darray_size(ir);
  ir = _parse_re_unit(ps, chunk, p, ir, icase);
  if (darray_size(ir) == s) {
    return ir;
  }

  int32_t qid = T(p).term_id;
  if (qid != LIT_QUESTION && qid != LIT_PLUS && qid != LIT_STAR) {
    return ir;
  }

  Location qloc = _loc(ps, &T(p));
  (*p)++;
  ReIrOp op = {RE_IR_LPAREN, 0, 0, qloc.line, qloc.col};
  ir = darray_insert(ir, s, &op);
  if (qid == LIT_STAR || qid == LIT_PLUS) {
    ir = re_ir_emit(ir, RE_IR_LOOP_BACK, 0, 0, qloc.line, qloc.col);
  }
  if (qid == LIT_STAR || qid == LIT_QUESTION) {
    ir = re_ir_emit(ir, RE_IR_FORK, 0, 0, qloc.line, qloc.col);
  }
  return re_ir_emit(ir, RE_IR_RPAREN, 0, 0, qloc.line, qloc.col);
}

static bool _is_re_unit(int32_t id) {
  return id == LIT_LPAREN || id == SCOPE_CHARCLASS || id == TOK_RE_REF || id == TOK_RE_DOT ||
         id == TOK_RE_SPACE_CLASS || id == TOK_RE_WORD_CLASS || id == TOK_RE_DIGIT_CLASS || id == TOK_RE_HEX_CLASS ||
         _is_str_char(id);
}

// re = re_quantified+<"|">
static ReIr _parse_re_expr(ParseState* ps, TokenChunk* chunk, int32_t* p, ReIr ir, bool icase) {
  while (_is_re_unit(T(p).term_id)) {
    ir = _parse_re_quantified(ps, chunk, p, ir, icase);
  }
  while (_eat(chunk, p, LIT_OR)) {
    Location oloc = _loc(ps, &chunk->tokens[*p - 1]);
    ir = re_ir_emit(ir, RE_IR_FORK, 0, 0, oloc.line, oloc.col);
    while (_is_re_unit(T(p).term_id)) {
      ir = _parse_re_quantified(ps, chunk, p, ir, icase);
    }
  }
  return ir;
}

static bool _parse_re(ParseState* ps, TokenChunk* chunk) {
  int32_t p = 0;
  chunk->value = _parse_re_expr(ps, chunk, &p, re_ir_new(), ps->shared->re_mode_icase);
  return true;
}

static bool _parse_re_str(ParseState* ps, TokenChunk* chunk) {
  _parse_peg_str(ps, chunk);
  ReIr ir = re_ir_new();
  for (size_t i = 0; i < darray_size(chunk->tokens); i++) {
    Token* t = &chunk->tokens[i];
    if (_is_str_char(t->term_id)) {
      Location loc = _loc(ps, t);
      int32_t cp = _decode_cp(ps->src, t);
      ir = re_ir_emit(ir, RE_IR_APPEND_CH, cp, cp, loc.line, loc.col);
    }
  }
  chunk->value = ir;
  return true;
}

static bool _parse_peg_str(ParseState* ps, TokenChunk* chunk) {
  char* buf = darray_new(sizeof(char), 0);
  for (size_t i = 0; i < darray_size(chunk->tokens); i++) {
    Token* t = &chunk->tokens[i];
    if (!_is_str_char(t->term_id)) {
      return false;
    }
    if (t->term_id == TOK_C_ESCAPE) {
      darray_push(buf, (char)_decode_cp(ps->src, t));
    } else if (t->term_id == TOK_CODEPOINT) {
      char enc[4] = {0};
      ustr_encode_utf8(enc, _decode_cp(ps->src, t));
      for (int32_t j = 0; enc[j]; j++) {
        darray_push(buf, enc[j]);
      }
    } else { // TOK_CHAR or TOK_PLAIN_ESCAPE
      UstrCpBuf sl = ustr_slice_cp(ps->src, t->term_id == TOK_PLAIN_ESCAPE ? t->cp_start + 1 : t->cp_start);
      for (int32_t j = 0; sl.buf[j]; j++) {
        darray_push(buf, sl.buf[j]);
      }
    }
  }
  darray_push(buf, '\0');
  chunk->aux_value = strdup(buf);
  darray_del(buf);
  return true;
}

// ============================================================================
// VPA recursive descent
// ============================================================================

static int32_t _intern_hook(int32_t tok_id) {
  switch (tok_id) {
  case TOK_HOOK_BEGIN:
    return HOOK_ID_BEGIN;
  case TOK_HOOK_END:
    return HOOK_ID_END;
  case TOK_HOOK_FAIL:
    return HOOK_ID_FAIL;
  case TOK_HOOK_UNPARSE:
    return HOOK_ID_UNPARSE;
  case TOK_HOOK_NOOP:
    return HOOK_ID_NOOP;
  default:
    return -1;
  }
}

// action = [ @tok_id | hooks | @user_hook_id ]
static void _parse_actions(ParseState* ps, TokenChunk* chunk, int32_t* p, VpaUnit* u) {
  for (;;) {
    Token* t = &T(p);
    int32_t au;
    if (t->term_id == TOK_TOK_ID) {
      au = _intern_tok(&ps->tokens, ps->src, t);
    } else if (t->term_id == TOK_USER_HOOK_ID) {
      au = -_intern_tok(&ps->hooks, ps->src, t);
    } else {
      int32_t hid = _intern_hook(t->term_id);
      if (hid < 0) {
        break;
      }
      au = -hid;
    }
    (*p)++;
    if (!u->action_units) {
      u->action_units = darray_new(sizeof(int32_t), 0);
    }
    darray_push(u->action_units, au);
  }
}

// take ReIr from a SCOPE_RE token's chunk
static void _take_re(ParseState* ps, TokenChunk* chunk, int32_t* p, VpaUnit* u) {
  Token* t = &T(p);
  (*p)++;
  u->kind = VPA_RE;
  TokenChunk* rc = _chunk(ps, t);
  u->re = (ReIr)_take(rc);
  u->binary_mode = ps->shared->re_mode_binary;
}

// take ReIr from a SCOPE_RE_STR token's chunk
static void _take_re_str(ParseState* ps, TokenChunk* chunk, int32_t* p, VpaUnit* u) {
  Token* t = &T(p);
  (*p)++;
  u->kind = VPA_RE;
  TokenChunk* sc = _chunk(ps, t);
  u->re = (ReIr)_take(sc);
  XFREE(sc->aux_value);
  sc->aux_value = NULL;
}

// scope_line = [ re action* | re_str action* | @pseudo_frag_eof action* | @re_frag_id action* | @vpa_id action* |
// @module_id ]
static bool _parse_scope_line(ParseState* ps, TokenChunk* chunk, int32_t* p, VpaUnit** children) {
  Token* t = &T(p);
  if (!t->term_id) {
    return false;
  }

  VpaUnit u = {0};
  Location loc = _loc(ps, t);
  u.source_line = loc.line;
  u.source_col = loc.col;

  if (t->term_id == SCOPE_RE) {
    _take_re(ps, chunk, p, &u);
    _parse_actions(ps, chunk, p, &u);
  } else if (t->term_id == SCOPE_RE_STR) {
    _take_re_str(ps, chunk, p, &u);
    _parse_actions(ps, chunk, p, &u);
  } else if (t->term_id == TOK_PSEUDO_FRAG_EOF) {
    (*p)++;
    u.kind = VPA_EOF;
    _parse_actions(ps, chunk, p, &u);
  } else if (t->term_id == TOK_RE_FRAG_ID) {
    (*p)++;
    u.kind = VPA_RE;
    u.re = re_ir_new();
    u.re = re_ir_emit(u.re, RE_IR_FRAG_REF, _frag_id(ps, t), 0, loc.line, loc.col);
    _parse_actions(ps, chunk, p, &u);
  } else if (t->term_id == TOK_VPA_ID) {
    (*p)++;
    u.kind = VPA_CALL;
    u.call_scope_id = _intern_tok(&ps->scope_names, ps->src, t);
    _parse_actions(ps, chunk, p, &u);
  } else if (t->term_id == TOK_MODULE_ID) {
    (*p)++;
    u.kind = VPA_MACRO_REF;
    u.macro_name = _tok_str(ps, t);
  } else {
    _error_at(ps, t, "unexpected token in scope body");
    return false;
  }
  darray_push(*children, u);
  return true;
}

// scope = @nl* scope_line+<@nl> @nl*
static bool _parse_scope(ParseState* ps, TokenChunk* chunk) {
  VpaUnit* children = darray_new(sizeof(VpaUnit), 0);
  int32_t p = 0;
  _skip_nl(chunk, &p);
  while (T(&p).term_id) {
    if (!_parse_scope_line(ps, chunk, &p, &children)) {
      chunk->value = children;
      return false;
    }
    _skip_nl(chunk, &p);
  }
  chunk->value = children;
  return true;
}

// lit_scope = @nl* re_str+<@nl> @nl*
static bool _parse_lit_scope(ParseState* ps, TokenChunk* chunk) {
  VpaUnit* children = darray_new(sizeof(VpaUnit), 0);
  int32_t p = 0;
  _skip_nl(chunk, &p);
  while (T(&p).term_id) {
    if (T(&p).term_id == TOK_NL) {
      _skip_nl(chunk, &p);
      continue;
    }
    Token* t = &T(&p);
    if (t->term_id != SCOPE_RE_STR) {
      _error_at(ps, t, "expected string");
      chunk->value = children;
      return false;
    }
    p++;
    TokenChunk* sc = _chunk(ps, t);
    Location loc = _loc(ps, t);
    VpaUnit u = {.kind = VPA_RE, .re = (ReIr)_take(sc), .source_line = loc.line, .source_col = loc.col};
    char* str = _take_aux(sc);
    u.action_units = darray_new(sizeof(int32_t), 0);
    int32_t tok_id = symtab_intern_f(&ps->tokens, "@lit.%s", str);
    XFREE(str);
    darray_push(u.action_units, tok_id);
    darray_push(children, u);
  }
  chunk->value = children;
  return true;
}

// ignore_toks = "%ignore" @tok_id+
static bool _parse_ignore_toks(ParseState* ps, TokenChunk* chunk, int32_t* p) {
  if (!_eat(chunk, p, LIT_IGNORE)) {
    return false;
  }
  if (T(p).term_id != TOK_TOK_ID) {
    _error_at(ps, &T(p), "expected @tok_id");
    return false;
  }
  while (T(p).term_id == TOK_TOK_ID) {
    if (!ps->ignores.names.offsets) {
      symtab_init(&ps->ignores.names, 0);
    }
    _intern_tok(&ps->ignores.names, ps->src, &T(p));
    (*p)++;
  }
  return true;
}

// effect_spec = "%effect" @user_hook_id "=" effect+<"|">
static bool _parse_effect_spec(ParseState* ps, TokenChunk* chunk, int32_t* p) {
  if (!_eat(chunk, p, LIT_EFFECT)) {
    return false;
  }
  if (T(p).term_id != TOK_USER_HOOK_ID) {
    _error_at(ps, &T(p), "expected @user_hook_id");
    return false;
  }
  Token* hook = &T(p);
  (*p)++;
  if (!_eat(chunk, p, LIT_EQ)) {
    _error_at(ps, &T(p), "expected '='");
    return false;
  }

  if (!ps->effect_decls) {
    ps->effect_decls = darray_new(sizeof(EffectDecl), 0);
  }
  EffectDecl ed = {.hook_id = _intern_tok(&ps->hooks, ps->src, hook), .effects = darray_new(sizeof(int32_t), 0)};

  for (;;) {
    Token* t = &T(p);
    int32_t au;
    if (t->term_id == TOK_TOK_ID) {
      au = _intern_tok(&ps->tokens, ps->src, t);
    } else {
      int32_t hid = _intern_hook(t->term_id);
      if (hid < 0) {
        _error_at(ps, t, "expected effect");
        darray_del(ed.effects);
        return false;
      }
      au = -hid;
    }
    (*p)++;
    darray_push(ed.effects, au);
    if (!_eat(chunk, p, LIT_OR)) {
      break;
    }
  }
  darray_push(ps->effect_decls, ed);
  return true;
}

// define_frag = "%define" @re_frag_id re
static bool _parse_define_frag(ParseState* ps, TokenChunk* chunk, int32_t* p) {
  if (!_eat(chunk, p, LIT_DEFINE)) {
    return false;
  }
  if (T(p).term_id != TOK_RE_FRAG_ID) {
    _error_at(ps, &T(p), "expected @re_frag_id");
    return false;
  }
  Token* name = &T(p);
  (*p)++;
  char* name_str = _tok_str(ps, name);
  if (strcmp(name_str, "EOF") == 0) {
    XFREE(name_str);
    _error_at(ps, name, "'EOF' is reserved and cannot be used as a fragment name");
    return false;
  }
  XFREE(name_str);
  if (T(p).term_id != SCOPE_RE) {
    _error_at(ps, &T(p), "expected re scope");
    return false;
  }
  Token* sc = &T(p);
  (*p)++;

  int32_t fid = _frag_id(ps, name);
  if (!ps->re_frags) {
    ps->re_frags = darray_new(sizeof(ReIr), 0);
  }
  while ((int32_t)darray_size(ps->re_frags) <= fid) {
    darray_push(ps->re_frags, (ReIr)NULL);
  }
  ps->re_frags[fid] = (ReIr)_take(_chunk(ps, sc));
  return true;
}

static VpaScope* _new_scope(ParseState* ps, char* name) {
  if (!ps->vpa_scopes) {
    ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);
  }
  int32_t id = symtab_intern(&ps->scope_names, name);
  darray_push(ps->vpa_scopes, ((VpaScope){.scope_id = id, .name = name}));
  return &ps->vpa_scopes[darray_size(ps->vpa_scopes) - 1];
}

// vpa_rule = @vpa_id "=" [ re | re_str | @pseudo_frag_eof | @re_frag_id ] action* scope?
static bool _parse_vpa_rule(ParseState* ps, TokenChunk* chunk, int32_t* p) {
  if (T(p).term_id != TOK_VPA_ID) {
    return false;
  }
  Token* name = &T(p);
  (*p)++;
  if (!_eat(chunk, p, LIT_EQ)) {
    _error_at(ps, &T(p), "expected '='");
    return false;
  }

  VpaScope* scope = _new_scope(ps, _tok_str(ps, name));
  Location loc = _loc(ps, name);
  scope->source_line = loc.line;
  scope->source_col = loc.col;

  Token* t = &T(p);
  // bare scope: only "main" can omit the regex pattern
  if (t->term_id == SCOPE_SCOPE) {
    if (strcmp(scope->name, "main") != 0) {
      _error_at(ps, name, "only 'main' can use bare scope");
      return false;
    }
    (*p)++;
    TokenChunk* sc = _chunk(ps, t);
    scope->children = (VpaUnit*)_take(sc);
    return true;
  }

  VpaUnit leader = {0};
  if (t->term_id == SCOPE_RE) {
    _take_re(ps, chunk, p, &leader);
  } else if (t->term_id == SCOPE_RE_STR) {
    _take_re_str(ps, chunk, p, &leader);
  } else if (t->term_id == TOK_PSEUDO_FRAG_EOF) {
    (*p)++;
    leader.kind = VPA_EOF;
  } else if (t->term_id == TOK_RE_FRAG_ID) {
    (*p)++;
    leader.kind = VPA_RE;
    leader.re = re_ir_new();
    leader.re = re_ir_emit(leader.re, RE_IR_FRAG_REF, _frag_id(ps, t), 0, loc.line, loc.col);
  } else {
    _error_at(ps, t, "expected re, string, EOF, or fragment ref");
    return false;
  }

  _parse_actions(ps, chunk, p, &leader);

  // scope?
  if (T(p).term_id == SCOPE_SCOPE) {
    Token* sc_tok = &T(p);
    (*p)++;
    scope->leader = leader;
    scope->children = (VpaUnit*)_take(_chunk(ps, sc_tok));
    return true;
  }
  if (!scope->children) {
    scope->children = darray_new(sizeof(VpaUnit), 0);
  }
  darray_push(scope->children, leader);
  return true;
}

// vpa_module_rule = [ @module_id "=" scope | @module_id "=" lit_scope ]
static bool _parse_vpa_module_rule(ParseState* ps, TokenChunk* chunk, int32_t* p) {
  if (T(p).term_id != TOK_MODULE_ID) {
    return false;
  }
  Token* name = &T(p);
  (*p)++;
  if (!_eat(chunk, p, LIT_EQ)) {
    _error_at(ps, &T(p), "expected '='");
    return false;
  }

  VpaScope* scope = _new_scope(ps, _tok_str(ps, name));
  Location loc = _loc(ps, name);
  scope->source_line = loc.line;
  scope->source_col = loc.col;
  scope->is_macro = true;

  Token* t = &T(p);
  if (t->term_id == SCOPE_SCOPE || t->term_id == SCOPE_LIT_SCOPE) {
    (*p)++;
    scope->children = (VpaUnit*)_take(_chunk(ps, t));
    return true;
  }
  _error_at(ps, t, "expected scope or lit_scope");
  return false;
}

static bool _validate_re_unit(ParseState* ps, VpaUnit* u) {
  if (!u->re || u->kind != VPA_RE) {
    return true;
  }
  ReIrValidateResult res = re_ir_validate(u->re, ps->re_frags);
  if (res.err_type == RE_IR_OK) {
    return true;
  }
  const char* name = NULL;
  if (res.err_type == RE_IR_ERR_RECURSION || res.err_type == RE_IR_ERR_MISSING_FRAG_ID) {
    name = symtab_get(&ps->re_frag_names, res.frag_id);
  }
  if (res.err_type == RE_IR_ERR_RECURSION) {
    parse_error(ps, "%d:%d: recursive fragment reference '%s'", res.line, res.col, name ? name : "?");
  } else if (res.err_type == RE_IR_ERR_MISSING_FRAG_ID) {
    parse_error(ps, "%d:%d: undefined fragment reference '%s'", res.line, res.col, name ? name : "?");
  }
  return false;
}

static bool _validate_all_re_ir(ParseState* ps) {
  for (size_t i = 0; i < darray_size(ps->vpa_scopes); i++) {
    VpaScope* scope = &ps->vpa_scopes[i];
    if (!_validate_re_unit(ps, &scope->leader)) {
      return false;
    }
    for (size_t j = 0; j < darray_size(scope->children); j++) {
      if (!_validate_re_unit(ps, &scope->children[j])) {
        return false;
      }
    }
  }
  return true;
}

// vpa_line = [ ignore_toks | effect_spec | define_frag | vpa_rule | vpa_module_rule ]
static bool _parse_vpa_line(ParseState* ps, TokenChunk* chunk, int32_t* p) {

  bool (*parsers[])(ParseState*, TokenChunk*, int32_t*) = {_parse_ignore_toks, _parse_effect_spec, _parse_define_frag,
                                                           _parse_vpa_rule, _parse_vpa_module_rule};
  for (int32_t i = 0; i < 5; i++) {
    if (parsers[i](ps, chunk, p)) {
      return true;
    }
    if (parse_has_error(ps)) {
      return false;
    }
  }
  _error_at(ps, &T(p), "unexpected token in vpa section");
  return false;
}

// vpa = @nl* vpa_line+<@nl> @nl*
// vpa = @nl* vpa_line+<@nl> @nl*
static bool _parse_vpa(ParseState* ps, TokenChunk* chunk) {
  int32_t p = 0;
  _skip_nl(chunk, &p);
  while (T(&p).term_id) {
    if (!_parse_vpa_line(ps, chunk, &p)) {
      return false;
    }
    _skip_nl(chunk, &p);
  }
  return _validate_all_re_ir(ps);
}

// ============================================================================
// PEG recursive descent
// ============================================================================

static bool _parse_seq(ParseState* ps, TokenChunk* chunk, int32_t* p, PegUnit* seq);

// resolve @peg_id: scope_name → PEG_TERM, else → PEG_CALL
static void _resolve_peg_id(ParseState* ps, Token* t, PegUnit* u) {
  char* name = _tok_str(ps, t);
  int32_t sid = symtab_find(&ps->scope_names, name);
  if (sid >= 0) {
    u->kind = PEG_TERM;
    u->id = sid;
  } else {
    u->kind = PEG_CALL;
    u->id = symtab_intern(&ps->rule_names, name);
  }
  XFREE(name);
}

// resolve peg_str scope → @lit.X term
static void _resolve_peg_str(ParseState* ps, Token* t, PegUnit* u) {
  TokenChunk* sc = _chunk(ps, t);
  u->kind = PEG_TERM;
  char* str = _take_aux(sc);
  u->id = symtab_intern_f(&ps->tokens, "@lit.%s", str);
  XFREE(str);
}

// parse one of: @peg_id | @peg_tok_id | peg_str — returns true if matched
static bool _parse_peg_atom(ParseState* ps, TokenChunk* chunk, int32_t* p, PegUnit* u) {
  Token* t = &T(p);
  if (t->term_id == TOK_PEG_ID) {
    (*p)++;
    _resolve_peg_id(ps, t, u);
    return true;
  }
  if (t->term_id == TOK_PEG_TOK_ID) {
    (*p)++;
    u->kind = PEG_TERM;
    u->id = _intern_tok(&ps->tokens, ps->src, t);
    return true;
  }
  if (t->term_id == SCOPE_PEG_STR) {
    (*p)++;
    _resolve_peg_str(ps, t, u);
    return true;
  }
  return false;
}

// interlace = "<" interlace_rhs ">"
// interlace_rhs = [ @peg_id | @peg_tok_id | peg_str ]
static bool _parse_interlace(ParseState* ps, TokenChunk* chunk, int32_t* p, PegUnit* u) {
  if (!_eat(chunk, p, LIT_INTERLACE_BEGIN)) {
    return false;
  }
  Token* t = &T(p);
  if (t->term_id == TOK_PEG_ID) {
    (*p)++;
    char* name = _tok_str(ps, t);
    int32_t sid = symtab_find(&ps->scope_names, name);
    u->interlace_rhs_kind = sid >= 0 ? PEG_TERM : PEG_CALL;
    u->interlace_rhs_id = sid >= 0 ? sid : symtab_intern(&ps->rule_names, name);
    XFREE(name);
  } else if (t->term_id == TOK_PEG_TOK_ID) {
    (*p)++;
    u->interlace_rhs_kind = PEG_TERM;
    u->interlace_rhs_id = _intern_tok(&ps->tokens, ps->src, t);
  } else if (t->term_id == SCOPE_PEG_STR) {
    (*p)++;
    TokenChunk* sc = _chunk(ps, t);
    u->interlace_rhs_kind = PEG_TERM;
    char* str = _take_aux(sc);
    u->interlace_rhs_id = symtab_intern_f(&ps->tokens, "@lit.%s", str);
    XFREE(str);
  } else {
    _error_at(ps, t, "expected interlace rhs");
    return false;
  }
  if (!_eat(chunk, p, LIT_INTERLACE_END)) {
    _error_at(ps, &T(p), "expected '>'");
    return false;
  }
  return true;
}

// multiplier = [ "?" | "+" interlace? | "*" interlace? ]
static void _parse_multiplier(ParseState* ps, TokenChunk* chunk, int32_t* p, PegUnit* u) {
  if (_eat(chunk, p, LIT_QUESTION)) {
    u->multiplier = '?';
  } else if (_eat(chunk, p, LIT_PLUS)) {
    u->multiplier = '+';
    _parse_interlace(ps, chunk, p, u);
  } else if (_eat(chunk, p, LIT_STAR)) {
    u->multiplier = '*';
    _parse_interlace(ps, chunk, p, u);
  }
}

// take pre-parsed PegUnit from branches chunk
static void _take_branches(PegUnit* u, TokenChunk* child) {
  char la = u->lookahead;
  PegUnit* parsed = (PegUnit*)_take(child);
  *u = *parsed;
  if (la) {
    u->lookahead = la;
  }
  XFREE(parsed);
}

// peg_lookahead_unit = [ @peg_id | @peg_tok_id | peg_str | branches ]
static bool _parse_lookahead_unit(ParseState* ps, TokenChunk* chunk, int32_t* p, PegUnit* u) {
  if (_parse_peg_atom(ps, chunk, p, u)) {
    return true;
  }
  if (T(p).term_id == SCOPE_BRANCHES) {
    Token* t = &T(p);
    (*p)++;
    _take_branches(u, _chunk(ps, t));
    return true;
  }
  _error_at(ps, &T(p), "expected lookahead expression after & or !");
  return false;
}

// peg_unit = [ "&" la | "!" la | atom multiplier? | branches ]
static bool _parse_peg_unit(ParseState* ps, TokenChunk* chunk, int32_t* p, PegUnit* u, bool allow_branches) {
  if (_eat(chunk, p, LIT_AND)) {
    u->lookahead = '&';
    return _parse_lookahead_unit(ps, chunk, p, u);
  }
  if (_eat(chunk, p, LIT_NOT)) {
    u->lookahead = '!';
    return _parse_lookahead_unit(ps, chunk, p, u);
  }
  if (_parse_peg_atom(ps, chunk, p, u)) {
    _parse_multiplier(ps, chunk, p, u);
    return true;
  }
  if (allow_branches && T(p).term_id == SCOPE_BRANCHES) {
    Token* t = &T(p);
    (*p)++;
    _take_branches(u, _chunk(ps, t));
    return true;
  }
  return false;
}

static bool _is_peg_start(int32_t id, bool allow_branches) {
  return id == TOK_PEG_ID || id == TOK_PEG_TOK_ID || id == SCOPE_PEG_STR || id == LIT_AND || id == LIT_NOT ||
         (allow_branches && id == SCOPE_BRANCHES);
}

// seq = peg_unit+
static bool _parse_seq(ParseState* ps, TokenChunk* chunk, int32_t* p, PegUnit* seq) {
  int32_t start = *p;
  while (_is_peg_start(T(p).term_id, true)) {
    if (!seq->children) {
      seq->children = darray_new(sizeof(PegUnit), 0);
    }
    darray_push(seq->children, ((PegUnit){0}));
    if (!_parse_peg_unit(ps, chunk, p, &seq->children[darray_size(seq->children) - 1], true)) {
      return false;
    }
  }
  if (*p == start) {
    _error_at(ps, &T(p), "expected peg unit");
    return false;
  }
  if (seq->children && darray_size(seq->children) == 1) {
    PegUnit child = seq->children[0];
    darray_del(seq->children);
    *seq = child;
  }
  return true;
}

// branch_line = [ peg_simple_unit+ @tag_id? | @tag_id ]
static bool _parse_branch_line(ParseState* ps, TokenChunk* chunk, int32_t* p, PegUnit* br) {
  if (!br->children) {
    br->children = darray_new(sizeof(PegUnit), 0);
  }

  // alternative 2: standalone @tag_id
  if (T(p).term_id == TOK_TAG_ID) {
    darray_push(br->children, ((PegUnit){.kind = PEG_SEQ, .tag = _tok_str(ps, &T(p))}));
    (*p)++;
    return true;
  }

  // alternative 1: peg_simple_unit+ @tag_id?
  darray_push(br->children, ((PegUnit){.kind = PEG_SEQ}));
  PegUnit* b = &br->children[darray_size(br->children) - 1];
  int32_t start = *p;
  while (_is_peg_start(T(p).term_id, false)) {
    if (!b->children) {
      b->children = darray_new(sizeof(PegUnit), 0);
    }
    darray_push(b->children, ((PegUnit){0}));
    if (!_parse_peg_unit(ps, chunk, p, &b->children[darray_size(b->children) - 1], false)) {
      return false;
    }
  }
  if (*p == start) {
    _error_at(ps, &T(p), "expected peg unit in branch");
    return false;
  }
  if (b->children && darray_size(b->children) == 1) {
    PegUnit child = b->children[0];
    darray_del(b->children);
    *b = child;
  }
  if (T(p).term_id == TOK_TAG_ID) {
    b->tag = _tok_str(ps, &T(p));
    (*p)++;
  }
  return true;
}

// branches = @nl* branch_line+<@nl> @nl*
static bool _parse_branches(ParseState* ps, TokenChunk* chunk) {
  PegUnit* u = XCALLOC(1, sizeof(PegUnit));
  u->kind = PEG_BRANCHES;
  int32_t p = 0;
  _skip_nl(chunk, &p);
  while (T(&p).term_id) {
    if (!_parse_branch_line(ps, chunk, &p, u)) {
      chunk->value = u;
      return false;
    }
    _skip_nl(chunk, &p);
  }
  if (u->children && darray_size(u->children) == 1) {
    PegUnit child = u->children[0];
    darray_del(u->children);
    *u = child;
  }
  chunk->value = u;
  return true;
}

// peg_rule = @peg_id @peg_assign seq
static bool _parse_peg_rule(ParseState* ps, TokenChunk* chunk, int32_t* p) {
  if (T(p).term_id != TOK_PEG_ID) {
    _error_at(ps, &T(p), "expected peg rule name");
    return false;
  }
  Token* name_tok = &T(p);
  (*p)++;
  if (!_eat(chunk, p, LIT_EQ)) {
    _error_at(ps, &T(p), "expected '='");
    return false;
  }
  if (!ps->peg_rules) {
    ps->peg_rules = darray_new(sizeof(PegRule), 0);
  }
  int32_t gid = _intern_tok(&ps->rule_names, ps->src, name_tok);
  Location loc = _loc(ps, name_tok);
  darray_push(ps->peg_rules, ((PegRule){.global_id = gid,
                                        .scope_id = -1,
                                        .source_line = loc.line,
                                        .source_col = loc.col,
                                        .body = {.kind = PEG_SEQ}}));
  return _parse_seq(ps, chunk, p, &ps->peg_rules[darray_size(ps->peg_rules) - 1].body);
}

// peg = @nl* peg_rule+<@nl> @nl*
static bool _parse_peg(ParseState* ps, TokenChunk* chunk) {
  int32_t p = 0;
  _skip_nl(chunk, &p);
  while (T(&p).term_id) {
    if (!_parse_peg_rule(ps, chunk, &p)) {
      return false;
    }
    _skip_nl(chunk, &p);
  }
  return true;
}

// ============================================================================
// Free / lifecycle
// ============================================================================

static void _free_vpa_unit(VpaUnit* u) {
  re_ir_free(u->re);
  darray_del(u->action_units);
  XFREE(u->macro_name);
}

static void _free_peg_unit(PegUnit* u) {
  XFREE(u->tag);
  for (size_t i = 0; i < darray_size(u->children); i++) {
    _free_peg_unit(&u->children[i]);
  }
  darray_del(u->children);
}

static void _free_vpa_unit(VpaUnit* u);

static void _free_state(ParseState* ps) {
  if (ps->tree) {
    for (size_t i = 0; i < darray_size(ps->tree->table); i++) {
      TokenChunk* c = &ps->tree->table[i];
      if (!c->value) {
        continue;
      }
      if (c->scope_id == SCOPE_SCOPE || c->scope_id == SCOPE_LIT_SCOPE) {
        VpaUnit* children = (VpaUnit*)c->value;
        for (size_t j = 0; j < darray_size(children); j++) {
          _free_vpa_unit(&children[j]);
        }
        darray_del(children);
      } else if (c->scope_id == SCOPE_BRANCHES) {
        _free_peg_unit((PegUnit*)c->value);
        XFREE(c->value);
      } else {
        re_ir_free((ReIr)c->value);
      }
      c->value = NULL;
    }
  }
  tt_tree_del(ps->tree, false);
  for (size_t i = 0; i < darray_size(ps->vpa_scopes); i++) {
    XFREE(ps->vpa_scopes[i].name);
    _free_vpa_unit(&ps->vpa_scopes[i].leader);
    for (size_t j = 0; j < darray_size(ps->vpa_scopes[i].children); j++) {
      _free_vpa_unit(&ps->vpa_scopes[i].children[j]);
    }
    darray_del(ps->vpa_scopes[i].children);
  }
  darray_del(ps->vpa_scopes);
  for (size_t i = 0; i < darray_size(ps->peg_rules); i++) {
    _free_peg_unit(&ps->peg_rules[i].body);
  }
  darray_del(ps->peg_rules);
  for (size_t i = 0; i < darray_size(ps->re_frags); i++) {
    re_ir_free(ps->re_frags[i]);
  }
  darray_del(ps->re_frags);
  symtab_free(&ps->re_frag_names);
  for (size_t i = 0; i < darray_size(ps->effect_decls); i++) {
    darray_del(ps->effect_decls[i].effects);
  }
  darray_del(ps->effect_decls);
  symtab_free(&ps->ignores.names);
  symtab_free(&ps->tokens);
  symtab_free(&ps->hooks);
  symtab_free(&ps->scope_names);
  symtab_free(&ps->rule_names);
}

ParseState* parse_state_new(void) {
  ParseState* s = XCALLOC(1, sizeof(ParseState));
  return s;
}

void parse_state_del(ParseState* ps) {
  if (!ps) {
    return;
  }
  _free_state(ps);
  XFREE(ps);
}

const char* parse_get_error(ParseState* ps) { return ps ? ps->error : NULL; }

// ============================================================================
// main = vpa peg
// ============================================================================

bool parse_nest(ParseState* ps, const char* src) {
  if (!src) {
    parse_error(ps, "null input");
    return false;
  }
  ps->src = src;
  ps->src_len = ustr_size(src);
  ps->tree = tt_tree_new(src);

  symtab_init(&ps->tokens, TOK_START);
  symtab_init(&ps->hooks, 1);
  symtab_intern(&ps->hooks, ".begin");   // HOOK_ID_BEGIN = 1
  symtab_intern(&ps->hooks, ".end");     // HOOK_ID_END = 2
  symtab_intern(&ps->hooks, ".fail");    // HOOK_ID_FAIL = 3
  symtab_intern(&ps->hooks, ".unparse"); // HOOK_ID_UNPARSE = 4
  symtab_intern(&ps->hooks, ".noop");    // HOOK_ID_NOOP = 5
  symtab_init(&ps->scope_names, SCOPE_START);
  symtab_init(&ps->rule_names, 0);
  symtab_init(&ps->re_frag_names, 0);

  UstrIter it = {0};
  ustr_iter_init(&it, src, 0);
  LexCtx lctx = {.ps = ps, .tree = ps->tree, .cp_count = ps->src_len, .it = it};
  ps->shared = &lctx.shared;
  if (!_lex_scope(&lctx, SCOPE_MAIN)) {
    return false;
  }

  if (!ps->vpa_scopes) {
    parse_error(ps, "missing [[vpa]]");
    return false;
  }
  if (!ps->peg_rules) {
    parse_error(ps, "missing [[peg]]");
    return false;
  }
  return true;
}
