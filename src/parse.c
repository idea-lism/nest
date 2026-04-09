// specs/parse.md — structure matches 1:1 to specs/bootstrap.nest PEG
#include "parse.h"
#include "darray.h"
#include "re.h"
#include "re_ir.h"
#include "ustr.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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

// --- Error reporting ---

bool parse_has_error(ParseState* ps) { return ps->error[0] != '\0'; }

static void _error_at(ParseState* ps, Token* t, const char* fmt, ...) {
  if (parse_has_error(ps)) {
    return;
  }
  int32_t off = 0;
  if (t && ps->tree) {
    Location loc = tt_locate(ps->tree, t->cp_start);
    off = snprintf(ps->error, sizeof(ps->error), "%d:%d: ", loc.line + 1, loc.col + 1);
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

// --- String helpers ---

static char* _cp_strdup(const char* src, int32_t cp_start, int32_t cp_size) {
  UstrIter it = {0};
  ustr_iter_init(&it, src, cp_start);
  int32_t start_byte = it.byte_off;
  for (int32_t i = 0; i < cp_size; i++) {
    ustr_iter_next(&it);
  }
  int32_t byte_len = it.byte_off - start_byte;
  char* s = malloc((size_t)byte_len + 1);
  memcpy(s, src + start_byte, (size_t)byte_len);
  s[byte_len] = '\0';
  return s;
}

static char* _tok_str(ParseState* ps, Token* t) { return _cp_strdup(ps->src, t->cp_start, t->cp_size); }
static char* _tok_str_skip(ParseState* ps, Token* t, int32_t s) {
  return _cp_strdup(ps->src, t->cp_start + s, t->cp_size - s);
}

__attribute__((format(printf, 1, 2))) char* parse_sfmt(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int32_t len = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char* s = malloc((size_t)len + 1);
  va_start(ap, fmt);
  vsnprintf(s, (size_t)len + 1, fmt, ap);
  va_end(ap);
  return s;
}

void parse_set_str(char** dst, char* s) {
  free(*dst);
  *dst = s;
}

static int32_t _decode_cp(const char* src, Token* t) {
  UstrIter it = {0};
  ustr_iter_init(&it, src, t->cp_start);
  const char* p = src + it.byte_off;
  if (t->tok_id == TOK_CODEPOINT) {
    return re_hex_to_codepoint(p + 2, (size_t)(t->cp_size - 3));
  }
  if (t->tok_id == TOK_C_ESCAPE && t->cp_size >= 2) {
    int32_t esc = re_c_escape(p[1]);
    return esc >= 0 ? esc : (int32_t)(unsigned char)p[1];
  }
  if (t->tok_id == TOK_PLAIN_ESCAPE && t->cp_size >= 2) {
    ustr_iter_next(&it);
    return ustr_iter_next(&it);
  }
  return ustr_iter_next(&it);
}

static bool _is_str_char(int32_t id) {
  return id == TOK_CHAR || id == TOK_CODEPOINT || id == TOK_C_ESCAPE || id == TOK_PLAIN_ESCAPE;
}

// --- Pushdown automaton lexer ---

typedef bool (*ParseFunc)(ParseState*, TokenChunk*);

typedef struct {
  LexFunc lex_fn;
  ParseFunc parse_fn;
} ScopeConfig;

typedef struct {
  ParseState* ps;
  TokenTree* tree;
  int32_t cp_count;
  UstrIter it;
  SharedState shared;
} LexCtx;

static int32_t _next_cp(LexCtx* ctx) { return ctx->it.cp_idx >= ctx->cp_count ? -2 : ustr_iter_next(&ctx->it); }

// forward-declare parse_fn functions for ScopeConfig
static bool _parse_re(ParseState* ps, TokenChunk* chunk);
static bool _parse_charclass(ParseState* ps, TokenChunk* chunk);
static bool _parse_re_str(ParseState* ps, TokenChunk* chunk);
static bool _parse_peg_str(ParseState* ps, TokenChunk* chunk);

static void _lex_scope(LexCtx* ctx, ScopeId scope_id) {
  static const ScopeConfig configs[] = {
      [SCOPE_MAIN] = {lex_main, NULL},
      [SCOPE_VPA] = {lex_vpa, NULL},
      [SCOPE_SCOPE] = {lex_scope, NULL},
      [SCOPE_LIT_SCOPE] = {lex_lit_scope, NULL},
      [SCOPE_PEG] = {lex_peg, NULL},
      [SCOPE_BRANCHES] = {lex_branches, NULL},
      [SCOPE_PEG_TAG] = {lex_peg_tag, NULL},
      [SCOPE_RE] = {lex_re, _parse_re},
      [SCOPE_RE_REF] = {lex_re_ref, NULL},
      [SCOPE_CHARCLASS] = {lex_charclass, _parse_charclass},
      [SCOPE_RE_STR] = {lex_re_str, _parse_re_str},
      [SCOPE_PEG_STR] = {lex_peg_str, _parse_peg_str},
  };
  ScopeConfig cfg = configs[scope_id];

  tt_push(ctx->tree, scope_id);
  int32_t chunk_idx = (int32_t)(darray_size(ctx->tree->table) - 1);
  int32_t scope_cp_start = ctx->it.cp_idx;

  int64_t state = 0;
  int32_t last_action = 0;
  int32_t tok_start = ctx->it.cp_idx;

  while (!parse_has_error(ctx->ps) && (ctx->it.cp_idx < ctx->cp_count || last_action != 0)) {
    int32_t saved = ctx->it.cp_idx;
    LexResult r = cfg.lex_fn(state, _next_cp(ctx));

    if (r.action != LEX_ACTION_NOMATCH) {
      state = r.state;
      last_action = (int32_t)r.action;
      continue;
    }
    ustr_iter_init(&ctx->it, ctx->ps->src, saved);

    if (last_action == ACTION_END) {
      break;
    } else if (last_action == ACTION_UNPARSE_END) {
      ustr_iter_init(&ctx->it, ctx->ps->src, tok_start);
      break;
    } else if (last_action == ACTION_IGNORE) {
      // skip
    } else if (last_action == ACTION_SET_QUOTE_BEGIN) {
      ctx->shared.last_quote_cp = ustr_cp_at(ctx->ps->src, tok_start);
      int32_t child_scope = scope_id == SCOPE_PEG || scope_id == SCOPE_BRANCHES ? SCOPE_PEG_STR : SCOPE_RE_STR;
      _lex_scope(ctx, child_scope);
    } else if (last_action == ACTION_STR_CHECK_END) {
      if (ustr_cp_at(ctx->ps->src, tok_start) == ctx->shared.last_quote_cp) {
        break;
      }
      tt_add(ctx->tree, TOK_CHAR, tok_start, saved - tok_start, -1);
    } else if (last_action == ACTION_SET_RE_MODE_BEGIN) {
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
      _lex_scope(ctx, SCOPE_RE);
    } else if (last_action == ACTION_SET_CC_KIND_BEGIN) {
      ctx->shared.cc_kind_neg = false;
      UstrIter cc_it = {0};
      ustr_iter_init(&cc_it, ctx->ps->src, tok_start);
      for (int32_t i = tok_start; i < saved; i++) {
        if (ustr_iter_next(&cc_it) == '^') {
          ctx->shared.cc_kind_neg = true;
          break;
        }
      }
      _lex_scope(ctx, SCOPE_CHARCLASS);
    } else if (last_action > 0 && last_action < SCOPE_COUNT) {
      _lex_scope(ctx, last_action);
    } else if (last_action >= LIT_START) {
      tt_add(ctx->tree, last_action, tok_start, saved - tok_start, -1);
    } else if (last_action == 0) {
      if (saved >= ctx->cp_count) {
        break;
      }
      if (scope_id == SCOPE_MAIN) {
        _error_at(ctx->ps, NULL, "unexpected character at position %d", saved);
      }
      break;
    }

    if (saved >= ctx->cp_count) {
      break;
    }
    tok_start = saved;
    last_action = 0;
    state = 0;
  }

  TokenChunk* chunk = ctx->tree->current;
  tt_pop(ctx->tree);
  if (ctx->tree->current->scope_id != SCOPE_START) {
    tt_add(ctx->tree, scope_id, scope_cp_start, ctx->it.cp_idx - scope_cp_start, chunk_idx);
  }
  if (cfg.parse_fn) {
    cfg.parse_fn(ctx->ps, chunk);
  }
}

// --- Token cursor ---

static Token* _peek(TokenChunk* chunk, int32_t tpos) {
  return tpos < (int32_t)darray_size(chunk->tokens) ? &chunk->tokens[tpos] : NULL;
}
static Token* _next(TokenChunk* chunk, int32_t* tpos) {
  Token* t = _peek(chunk, *tpos);
  if (t) {
    (*tpos)++;
  }
  return t;
}
static bool _at_end(TokenChunk* chunk, int32_t tpos) { return !_peek(chunk, tpos); }
static bool _at(TokenChunk* chunk, int32_t tpos, int32_t id) {
  Token* t = _peek(chunk, tpos);
  return t && t->tok_id == id;
}
static void _skip_nl(TokenChunk* chunk, int32_t* tpos) {
  while (_at(chunk, *tpos, TOK_NL)) {
    (*tpos)++;
  }
}

static Token* _expect(ParseState* ps, TokenChunk* chunk, int32_t* tpos, int32_t id, const char* what) {
  Token* t = _peek(chunk, *tpos);
  if (!t || t->tok_id != id) {
    _error_at(ps, t, "expected %s", what);
    return NULL;
  }
  (*tpos)++;
  return t;
}

static TokenChunk* _scope_chunk(ParseState* ps, Token* t) { return &ps->tree->table[t->chunk_id]; }

// --- Fragment lookup ---

static ReIr _lookup_frag(ParseState* ps, Token* t) {
  UstrIter a = {0}, b = {0};
  ustr_iter_init(&a, ps->src, t->cp_start);
  ustr_iter_init(&b, ps->src, t->cp_start + t->cp_size);
  const char* p = ps->src + a.byte_off;
  int32_t len = b.byte_off - a.byte_off;
  for (int32_t i = 0; i < (int32_t)darray_size(ps->re_frags); i++) {
    if (strncmp(ps->re_frags[i].name, p, (size_t)len) == 0 && ps->re_frags[i].name[len] == '\0') {
      return re_ir_clone(ps->re_frags[i].re);
    }
  }
  _error_at(ps, t, "undefined fragment '%.*s'", len, p);
  return NULL;
}

// ============================================================================
// RE recursive descent — see specs/bootstrap.nest [[peg]] "Regex AST rules"
// ============================================================================

static ReIr _parse_re_expr(ParseState* ps, TokenChunk* chunk, int32_t* tpos, ReIr ir, bool icase);

// charclass_char
static bool _is_charclass_char(int32_t id) { return _is_str_char(id); }

static int32_t _parse_charclass_char(ParseState* ps, TokenChunk* chunk, int32_t* tpos) {
  return _decode_cp(ps->src, _next(chunk, tpos));
}

// charclass_unit = [ charclass_char @range_sep charclass_char : range | charclass_char : single ]
static ReIr _parse_charclass_unit(ParseState* ps, TokenChunk* chunk, int32_t* tpos, ReIr ir) {
  int32_t lo = _parse_charclass_char(ps, chunk, tpos);
  if (_at(chunk, *tpos, TOK_RANGE_SEP)) {
    _next(chunk, tpos);
    int32_t hi = _parse_charclass_char(ps, chunk, tpos);
    return re_ir_emit(ir, RE_IR_APPEND_CH, lo, hi);
  }
  return re_ir_emit(ir, RE_IR_APPEND_CH, lo, lo);
}

// charclass = charclass_unit+
static ReIr _parse_charclass_body(ParseState* ps, TokenChunk* chunk, int32_t* tpos, ReIr ir, bool neg, bool icase) {
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  if (neg) {
    ir = re_ir_emit(ir, RE_IR_RANGE_NEG, 0, 0);
  }
  while (!_at_end(chunk, *tpos) && _is_charclass_char(_peek(chunk, *tpos)->tok_id)) {
    ir = _parse_charclass_unit(ps, chunk, tpos, ir);
  }
  if (icase) {
    ir = re_ir_emit(ir, RE_IR_RANGE_IC, 0, 0);
  }
  return re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
}

static ReIr _emit_shorthand(ReIr ir, int32_t tok_id) {
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  switch (tok_id) {
  case TOK_RE_DOT:
    ir = re_ir_emit(ir, RE_IR_RANGE_NEG, 0, 0);
    ir = re_ir_emit(ir, RE_IR_APPEND_CH, '\n', '\n');
    break;
  case TOK_RE_SPACE_CLASS:
    ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_S, 0, 0);
    break;
  case TOK_RE_WORD_CLASS:
    ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_W, 0, 0);
    break;
  case TOK_RE_DIGIT_CLASS:
    ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_D, 0, 0);
    break;
  case TOK_RE_HEX_CLASS:
    ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_H, 0, 0);
    break;
  }
  return re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
}

static bool _is_re_unit(int32_t id) {
  return id == LIT_LPAREN || id == SCOPE_CHARCLASS || id == SCOPE_RE_REF || id == TOK_RE_DOT ||
         id == TOK_RE_SPACE_CLASS || id == TOK_RE_WORD_CLASS || id == TOK_RE_DIGIT_CLASS || id == TOK_RE_HEX_CLASS ||
         id == TOK_RE_BOF || id == TOK_RE_EOF || _is_str_char(id);
}

// re_unit = [ "(" re ")" | charclass | shorthand | @re_bof | @re_eof | @re_ref | char_token ]
static ReIr _parse_re_unit(ParseState* ps, TokenChunk* chunk, int32_t* tpos, ReIr ir, bool icase) {
  Token* t = _peek(chunk, *tpos);
  if (!t) {
    return ir;
  }
  switch (t->tok_id) {
  case LIT_LPAREN:
    _next(chunk, tpos);
    ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0);
    ir = _parse_re_expr(ps, chunk, tpos, ir, icase);
    if (_at(chunk, *tpos, LIT_RPAREN)) {
      _next(chunk, tpos);
    }
    return re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
  case SCOPE_CHARCLASS: {
    _next(chunk, tpos);
    TokenChunk* cc = _scope_chunk(ps, t);
    ReIr cc_ir = (ReIr)cc->value;
    if (cc_ir) {
      ir = darray_concat(ir, cc_ir);
    }
    return ir;
  }
  case TOK_RE_DOT:
  case TOK_RE_SPACE_CLASS:
  case TOK_RE_WORD_CLASS:
  case TOK_RE_DIGIT_CLASS:
  case TOK_RE_HEX_CLASS:
    _next(chunk, tpos);
    return _emit_shorthand(ir, t->tok_id);
  case TOK_RE_BOF:
    _next(chunk, tpos);
    return re_ir_emit_ch(ir, LEX_CP_BOF);
  case TOK_RE_EOF:
    _next(chunk, tpos);
    return re_ir_emit_ch(ir, LEX_CP_EOF);
  case SCOPE_RE_REF: {
    _next(chunk, tpos);
    TokenChunk* ref_chunk = _scope_chunk(ps, t);
    for (int32_t i = 0; i < (int32_t)darray_size(ref_chunk->tokens); i++) {
      if (ref_chunk->tokens[i].tok_id == TOK_RE_REF) {
        return re_ir_emit(ir, RE_IR_FRAG_REF, ref_chunk->tokens[i].cp_start, ref_chunk->tokens[i].cp_size);
      }
    }
    return ir;
  }
  case TOK_CHAR:
  case TOK_CODEPOINT:
  case TOK_C_ESCAPE:
  case TOK_PLAIN_ESCAPE: {
    _next(chunk, tpos);
    int32_t cp = _decode_cp(ps->src, t);
    return re_ir_emit(ir, icase ? RE_IR_APPEND_CH_IC : RE_IR_APPEND_CH, cp, cp);
  }
  default:
    return ir;
  }
}

// re_quantified = re_unit [ "?" | "+" | "*" | (none) ]
static ReIr _parse_re_quantified(ParseState* ps, TokenChunk* chunk, int32_t* tpos, ReIr ir, bool icase) {
  int32_t s = (int32_t)darray_size(ir);
  ir = _parse_re_unit(ps, chunk, tpos, ir, icase);
  int32_t e = (int32_t)darray_size(ir);
  if (s == e) {
    return ir;
  }

  Token* q = _peek(chunk, *tpos);
  if (!q) {
    return ir;
  }

  if (q->tok_id == LIT_QUESTION) {
    _next(chunk, tpos);
    ReIrOp op = {RE_IR_LPAREN, 0, 0};
    ir = darray_insert(ir, (size_t)s, &op);
    ir = re_ir_emit(ir, RE_IR_FORK, 0, 0);
    ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
  } else if (q->tok_id == LIT_PLUS) {
    _next(chunk, tpos);
    ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0);
    for (int32_t i = s; i < e; i++) {
      darray_push(ir, ir[i]);
    }
    ir = re_ir_emit(ir, RE_IR_FORK, 0, 0);
    ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
  } else if (q->tok_id == LIT_STAR) {
    _next(chunk, tpos);
    ReIrOp op = {RE_IR_LPAREN, 0, 0};
    ir = darray_insert(ir, (size_t)s, &op);
    for (int32_t i = s + 1; i <= e; i++) {
      darray_push(ir, ir[i]);
    }
    ir = re_ir_emit(ir, RE_IR_FORK, 0, 0);
    ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
  }
  return ir;
}

// re = re_quantified+<"|">
static ReIr _parse_re_expr(ParseState* ps, TokenChunk* chunk, int32_t* tpos, ReIr ir, bool icase) {
  while (!_at_end(chunk, *tpos) && _is_re_unit(_peek(chunk, *tpos)->tok_id)) {
    ir = _parse_re_quantified(ps, chunk, tpos, ir, icase);
  }
  while (_at(chunk, *tpos, LIT_OR)) {
    _next(chunk, tpos);
    ir = re_ir_emit(ir, RE_IR_FORK, 0, 0);
    while (!_at_end(chunk, *tpos) && _is_re_unit(_peek(chunk, *tpos)->tok_id)) {
      ir = _parse_re_quantified(ps, chunk, tpos, ir, icase);
    }
  }
  return ir;
}

static bool _parse_re(ParseState* ps, TokenChunk* chunk) {
  int32_t tpos = 0;
  chunk->value = _parse_re_expr(ps, chunk, &tpos, re_ir_new(), ps->shared->re_mode_icase);
  return true;
}

static bool _parse_charclass(ParseState* ps, TokenChunk* chunk) {
  int32_t tpos = 0;
  chunk->value =
      _parse_charclass_body(ps, chunk, &tpos, re_ir_new(), ps->shared->cc_kind_neg, ps->shared->re_mode_icase);
  return true;
}

// re_str scope → ReIr (each char becomes a literal match)
static bool _parse_re_str(ParseState* ps, TokenChunk* chunk) {
  ReIr ir = re_ir_new();
  for (int32_t i = 0; i < (int32_t)darray_size(chunk->tokens); i++) {
    Token* t = &chunk->tokens[i];
    if (_is_str_char(t->tok_id)) {
      ir = re_ir_emit_ch(ir, _decode_cp(ps->src, t));
    }
  }
  chunk->value = ir;
  return true;
}

// peg_str scope → owned string
static bool _parse_peg_str(ParseState* ps, TokenChunk* chunk) {
  char* buf = darray_new(sizeof(char), 0);
  for (int32_t i = 0; i < (int32_t)darray_size(chunk->tokens); i++) {
    Token* t = &chunk->tokens[i];
    if (!_is_str_char(t->tok_id)) {
      continue;
    }
    if (t->tok_id == TOK_C_ESCAPE) {
      darray_push(buf, (char)_decode_cp(ps->src, t));
    } else {
      UstrCpBuf sl = ustr_slice_cp(ps->src, t->tok_id == TOK_PLAIN_ESCAPE ? t->cp_start + 1 : t->cp_start);
      if (t->tok_id == TOK_CODEPOINT) {
        char enc[4] = {0};
        ustr_encode_utf8(enc, _decode_cp(ps->src, t));
        for (int32_t j = 0; enc[j]; j++) {
          darray_push(buf, enc[j]);
        }
      } else {
        for (int32_t j = 0; sl.buf[j]; j++) {
          darray_push(buf, sl.buf[j]);
        }
      }
    }
  }
  darray_push(buf, '\0');
  chunk->value = strdup(buf);
  darray_del(buf);
  return true;
}

// ============================================================================
// VPA recursive descent
// ============================================================================

static void _add_child(VpaScope* scope, VpaUnit unit) {
  if (!scope->children) {
    scope->children = darray_new(sizeof(VpaUnit), 0);
  }
  darray_push(scope->children, unit);
}

static int32_t _intern_hook(int32_t bootstrap_tok_id) {
  switch (bootstrap_tok_id) {
  case TOK_HOOK_BEGIN:
    return HOOK_ID_BEGIN;
  case TOK_HOOK_END:
    return HOOK_ID_END;
  case TOK_HOOK_FAIL:
    return HOOK_ID_FAIL;
  case TOK_HOOK_UNPARSE:
    return HOOK_ID_UNPARSE;
  default:
    return -1;
  }
}

static bool _is_action(int32_t id) {
  return id == TOK_TOK_ID || id == TOK_HOOK_BEGIN || id == TOK_HOOK_END || id == TOK_HOOK_FAIL ||
         id == TOK_HOOK_UNPARSE || id == TOK_USER_HOOK_ID;
}

// action = [ @tok_id | hooks | @user_hook_id ]
// Builds action_units on the unit.
static void _parse_actions(ParseState* ps, TokenChunk* chunk, int32_t* tpos, VpaUnit* u) {
  while (!_at_end(chunk, *tpos) && _is_action(_peek(chunk, *tpos)->tok_id)) {
    Token* t = _next(chunk, tpos);
    if (t->tok_id == TOK_TOK_ID) {
      char* tok_name = _tok_str_skip(ps, t, 1);
      int32_t tok_id = symtab_intern(&ps->tokens, tok_name);
      free(tok_name);
      if (!u->action_units) {
        u->action_units = darray_new(sizeof(int32_t), 0);
      }
      darray_push(u->action_units, tok_id);
    } else if (t->tok_id == TOK_USER_HOOK_ID) {
      char* hook_name = _tok_str(ps, t);
      int32_t hook_id = symtab_intern(&ps->hooks, hook_name);
      free(hook_name);
      if (!u->action_units) {
        u->action_units = darray_new(sizeof(int32_t), 0);
      }
      int32_t au = -hook_id;
      darray_push(u->action_units, au);
    } else {
      int32_t hook_id = _intern_hook(t->tok_id);
      if (hook_id >= 0) {
        if (!u->action_units) {
          u->action_units = darray_new(sizeof(int32_t), 0);
        }
        int32_t au = -hook_id;
        darray_push(u->action_units, au);
      }
    }
  }
}

static bool _consume_re(ParseState* ps, TokenChunk* chunk, int32_t* tpos, VpaUnit* u) {
  Token* sc = _peek(chunk, *tpos);
  if (!sc || sc->tok_id != SCOPE_RE) {
    _error_at(ps, sc, "expected re scope");
    return false;
  }
  _next(chunk, tpos);
  u->kind = VPA_RE;
  TokenChunk* re_chunk = _scope_chunk(ps, sc);
  u->re = (ReIr)re_chunk->value;
  re_chunk->value = NULL;
  u->binary_mode = ps->shared->re_mode_binary;
  return true;
}

// scope_line = [ re action* | re_str action* | @re_frag_id action* | @vpa_id action* | @module_id ]
static bool _parse_scope_line(ParseState* ps, TokenChunk* chunk, int32_t* tpos, VpaScope* scope) {
  Token* t = _peek(chunk, *tpos);
  if (!t) {
    return false;
  }

  VpaUnit u = {0};
  if (t->tok_id == SCOPE_RE) {
    if (!_consume_re(ps, chunk, tpos, &u)) {
      return false;
    }
    _parse_actions(ps, chunk, tpos, &u);
  } else if (t->tok_id == SCOPE_RE_STR) {
    _next(chunk, tpos);
    u.kind = VPA_RE;
    TokenChunk* sc = _scope_chunk(ps, t);
    u.re = (ReIr)sc->value;
    sc->value = NULL;
    _parse_actions(ps, chunk, tpos, &u);
  } else if (t->tok_id == TOK_RE_FRAG_ID) {
    _next(chunk, tpos);
    u.kind = VPA_RE;
    u.re = _lookup_frag(ps, t);
    if (!u.re) {
      return false;
    }
    _parse_actions(ps, chunk, tpos, &u);
  } else if (t->tok_id == TOK_VPA_ID) {
    _next(chunk, tpos);
    char* ref_name = _tok_str(ps, t);
    u.kind = VPA_CALL;
    u.call_scope_id = symtab_intern(&ps->scope_names, ref_name);
    free(ref_name);
    _parse_actions(ps, chunk, tpos, &u);
  } else if (t->tok_id == TOK_MODULE_ID) {
    _next(chunk, tpos);
    u.kind = VPA_MACRO_REF;
    u.macro_name = _tok_str_skip(ps, t, 1);
  } else {
    _error_at(ps, t, "unexpected token in scope body");
    return false;
  }
  _add_child(scope, u);
  return true;
}

// nl-separated lines within a scope chunk
static bool _parse_scope_body(ParseState* ps, TokenChunk* chunk, VpaScope* scope) {
  int32_t tpos = 0;
  _skip_nl(chunk, &tpos);
  while (!_at_end(chunk, tpos)) {
    if (!_parse_scope_line(ps, chunk, &tpos, scope)) {
      return false;
    }
    _skip_nl(chunk, &tpos);
  }
  return true;
}

// lit_scope = @nl* re_str+<@nl> @nl*
static bool _parse_lit_scope(ParseState* ps, TokenChunk* chunk, VpaScope* scope) {
  int32_t tpos = 0;
  _skip_nl(chunk, &tpos);
  while (!_at_end(chunk, tpos)) {
    Token* t = _peek(chunk, tpos);
    if (t->tok_id == TOK_NL) {
      _skip_nl(chunk, &tpos);
      continue;
    }
    if (t->tok_id != SCOPE_RE_STR) {
      _error_at(ps, t, "expected string");
      return false;
    }
    _next(chunk, &tpos);
    TokenChunk* sc = _scope_chunk(ps, t);
    ReIr re = (ReIr)sc->value;
    sc->value = NULL;

    VpaUnit u = {.kind = VPA_RE, .re = re};
    UstrByteSlice lit = ustr_slice_bytes(ps->src, t->cp_start, t->cp_size);
    int32_t tok_id = symtab_intern_f(&ps->tokens, "lit.%.*s", lit.size, lit.s);
    u.action_units = darray_new(sizeof(int32_t), 0);
    darray_push(u.action_units, tok_id);

    _add_child(scope, u);
  }
  return true;
}

// ignore_toks = "%ignore" @tok_id+
static bool _parse_ignore_toks(ParseState* ps, TokenChunk* chunk, int32_t* tpos) {
  if (!_at(chunk, *tpos, LIT_IGNORE)) {
    return false;
  }
  _next(chunk, tpos);
  if (!_at(chunk, *tpos, TOK_TOK_ID)) {
    _error_at(ps, _peek(chunk, *tpos), "expected @tok_id");
    return false;
  }
  while (_at(chunk, *tpos, TOK_TOK_ID)) {
    Token* t = _next(chunk, tpos);
    if (!ps->ignores.names.offsets) {
      symtab_init(&ps->ignores.names, 0);
    }
    char* n = _tok_str_skip(ps, t, 1);
    symtab_intern(&ps->ignores.names, n);
    free(n);
  }
  return true;
}

// effect_spec = "%effect" @user_hook_id "=" effect+<"|">
// effect = [ @tok_id | @hook_begin | @hook_end | @hook_fail | @hook_unparse ]
static bool _parse_effect_spec(ParseState* ps, TokenChunk* chunk, int32_t* tpos) {
  if (!_at(chunk, *tpos, LIT_EFFECT)) {
    return false;
  }
  _next(chunk, tpos);
  Token* hook = _expect(ps, chunk, tpos, TOK_USER_HOOK_ID, "@user_hook_id");
  if (!hook || !_expect(ps, chunk, tpos, LIT_EQ, "'='")) {
    return false;
  }

  if (!ps->effect_decls) {
    ps->effect_decls = darray_new(sizeof(EffectDecl), 0);
  }
  char* hook_name = _tok_str(ps, hook);
  int32_t hook_id = symtab_intern(&ps->hooks, hook_name);
  free(hook_name);
  EffectDecl ed = {.hook_id = hook_id, .effects = darray_new(sizeof(int32_t), 0)};

  for (;;) {
    Token* t = _peek(chunk, *tpos);
    if (!t || (t->tok_id != TOK_TOK_ID && t->tok_id != TOK_HOOK_BEGIN && t->tok_id != TOK_HOOK_END &&
               t->tok_id != TOK_HOOK_FAIL && t->tok_id != TOK_HOOK_UNPARSE)) {
      _error_at(ps, t, "expected effect");
      darray_del(ed.effects);
      return false;
    }
    _next(chunk, tpos);
    int32_t au;
    if (t->tok_id == TOK_TOK_ID) {
      char* tok_name = _tok_str_skip(ps, t, 1);
      au = symtab_intern(&ps->tokens, tok_name);
      free(tok_name);
    } else {
      au = -_intern_hook(t->tok_id);
    }
    darray_push(ed.effects, au);
    if (!_at(chunk, *tpos, LIT_OR)) {
      break;
    }
    _next(chunk, tpos);
  }
  darray_push(ps->effect_decls, ed);
  return true;
}

// define_frag = "%define" @re_frag_id re
static bool _parse_define_frag(ParseState* ps, TokenChunk* chunk, int32_t* tpos) {
  if (!_at(chunk, *tpos, LIT_DEFINE)) {
    return false;
  }
  _next(chunk, tpos);
  Token* name = _expect(ps, chunk, tpos, TOK_RE_FRAG_ID, "@re_frag_id");
  if (!name) {
    return false;
  }
  Token* sc = _expect(ps, chunk, tpos, SCOPE_RE, "re scope");
  if (!sc) {
    return false;
  }

  TokenChunk* re_chunk = _scope_chunk(ps, sc);
  ReFragment frag = {.name = _tok_str(ps, name), .re = (ReIr)re_chunk->value};
  re_chunk->value = NULL;
  if (!ps->re_frags) {
    ps->re_frags = darray_new(sizeof(ReFragment), 0);
  }
  darray_push(ps->re_frags, frag);
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

// vpa_rule = @vpa_id "=" [ re | peg_str | @re_frag_id ] action* scope?
static bool _parse_vpa_rule(ParseState* ps, TokenChunk* chunk, int32_t* tpos) {
  if (!_at(chunk, *tpos, TOK_VPA_ID)) {
    return false;
  }
  Token* name = _next(chunk, tpos);
  if (!_expect(ps, chunk, tpos, LIT_EQ, "'='")) {
    return false;
  }

  VpaScope* scope = _new_scope(ps, _tok_str(ps, name));

  Token* t = _peek(chunk, *tpos);
  if (!t) {
    _error_at(ps, name, "expected pattern or scope");
    return false;
  }

  // bare scope: only "main" can omit the regex pattern
  if (t->tok_id == SCOPE_SCOPE) {
    if (strcmp(scope->name, "main") != 0) {
      _error_at(ps, name, "only 'main' can use bare scope");
      return false;
    }
    _next(chunk, tpos);
    return _parse_scope_body(ps, _scope_chunk(ps, t), scope);
  }

  VpaUnit leader = {0};
  if (t->tok_id == SCOPE_RE) {
    if (!_consume_re(ps, chunk, tpos, &leader)) {
      return false;
    }
  } else if (t->tok_id == SCOPE_RE_STR) {
    _next(chunk, tpos);
    leader.kind = VPA_RE;
    TokenChunk* sc = _scope_chunk(ps, t);
    leader.re = (ReIr)sc->value;
    sc->value = NULL;
  } else if (t->tok_id == TOK_RE_FRAG_ID) {
    _next(chunk, tpos);
    leader.kind = VPA_RE;
    leader.re = _lookup_frag(ps, t);
    if (!leader.re) {
      return false;
    }
  } else {
    _error_at(ps, t, "expected re, string, or fragment ref");
    return false;
  }

  _parse_actions(ps, chunk, tpos, &leader);

  Token* sc = _peek(chunk, *tpos);
  if (sc && sc->tok_id == SCOPE_SCOPE) {
    _next(chunk, tpos);
    scope->leader = leader;
    return _parse_scope_body(ps, _scope_chunk(ps, sc), scope);
  }
  // non-scope rule: store pattern as a child
  _add_child(scope, leader);
  return true;
}

// vpa_module_rule = [ @module_id "=" scope | @module_id "=" lit_scope ]
static bool _parse_vpa_module_rule(ParseState* ps, TokenChunk* chunk, int32_t* tpos) {
  if (!_at(chunk, *tpos, TOK_MODULE_ID)) {
    return false;
  }
  Token* name = _next(chunk, tpos);
  if (!_expect(ps, chunk, tpos, LIT_EQ, "'='")) {
    return false;
  }

  VpaScope* scope = _new_scope(ps, _tok_str_skip(ps, name, 1));
  scope->is_macro = true;

  Token* t = _peek(chunk, *tpos);
  if (t && t->tok_id == SCOPE_SCOPE) {
    _next(chunk, tpos);
    return _parse_scope_body(ps, _scope_chunk(ps, t), scope);
  }
  if (t && t->tok_id == SCOPE_LIT_SCOPE) {
    _next(chunk, tpos);
    return _parse_lit_scope(ps, _scope_chunk(ps, t), scope);
  }
  _error_at(ps, t, "expected scope or lit_scope");
  return false;
}

// vpa_line = [ ignore_toks | effect_spec | define_frag | vpa_rule | vpa_module_rule ]
static bool _parse_vpa_line(ParseState* ps, TokenChunk* chunk, int32_t* tpos) {
  bool (*parsers[])(ParseState*, TokenChunk*, int32_t*) = {_parse_ignore_toks, _parse_effect_spec, _parse_define_frag,
                                                           _parse_vpa_rule, _parse_vpa_module_rule};
  for (int32_t i = 0; i < 5; i++) {
    if (parsers[i](ps, chunk, tpos)) {
      return true;
    }
    if (parse_has_error(ps)) {
      return false;
    }
  }
  _error_at(ps, _peek(chunk, *tpos), "unexpected token in vpa section");
  return false;
}

// vpa = @nl* vpa_line+<@nl> @nl*
static bool _parse_vpa(ParseState* ps, TokenChunk* chunk, int32_t* tpos) {
  _skip_nl(chunk, tpos);
  while (!_at_end(chunk, *tpos)) {
    if (!_parse_vpa_line(ps, chunk, tpos)) {
      return false;
    }
    _skip_nl(chunk, tpos);
  }
  return true;
}

// ============================================================================
// PEG recursive descent
// ============================================================================

static bool _parse_seq(ParseState* ps, TokenChunk* chunk, int32_t* tpos, PegUnit* seq);

static bool _is_peg_unit(int32_t id) {
  return id == TOK_PEG_ID || id == TOK_PEG_TOK_ID || id == SCOPE_PEG_STR || id == SCOPE_BRANCHES;
}

// interlace_rhs = [ @peg_id | @peg_tok_id | peg_str ]
// interlace = "<" interlace_rhs ">"
static bool _parse_interlace(ParseState* ps, TokenChunk* chunk, int32_t* tpos, PegUnit* u) {
  if (!_at(chunk, *tpos, LIT_INTERLACE_BEGIN)) {
    return false;
  }
  _next(chunk, tpos);

  Token* t = _peek(chunk, *tpos);
  if (!t) {
    _error_at(ps, t, "expected interlace rhs");
    return false;
  }

  if (t->tok_id == TOK_PEG_ID) {
    _next(chunk, tpos);
    char* name = _tok_str(ps, t);
    u->interlace_rhs_kind = PEG_CALL;
    u->interlace_rhs_id = symtab_intern(&ps->rule_names, name);
    free(name);
  } else if (t->tok_id == TOK_PEG_TOK_ID) {
    _next(chunk, tpos);
    char* name = _tok_str_skip(ps, t, 1);
    u->interlace_rhs_kind = PEG_TOK;
    u->interlace_rhs_id = symtab_intern(&ps->tokens, name);
    free(name);
  } else if (t->tok_id == SCOPE_PEG_STR) {
    _next(chunk, tpos);
    TokenChunk* sc = _scope_chunk(ps, t);
    u->interlace_rhs_kind = PEG_TOK;
    u->interlace_rhs_id = symtab_intern(&ps->tokens, sc->value);
    free(sc->value);
    sc->value = NULL;
  } else {
    _error_at(ps, t, "expected @peg_id, @peg_tok_id, or peg_str in interlace");
    return false;
  }

  return !!_expect(ps, chunk, tpos, LIT_INTERLACE_END, "'>'");
}

// multiplier = [ "?" | "+" interlace? | "*" interlace? ]
static bool _parse_multiplier(ParseState* ps, TokenChunk* chunk, int32_t* tpos, PegUnit* u) {
  Token* t = _peek(chunk, *tpos);
  if (!t) {
    return true;
  }
  if (t->tok_id == LIT_QUESTION) {
    _next(chunk, tpos);
    u->multiplier = '?';
  } else if (t->tok_id == LIT_PLUS) {
    _next(chunk, tpos);
    u->multiplier = '+';
    _parse_interlace(ps, chunk, tpos, u);
  } else if (t->tok_id == LIT_STAR) {
    _next(chunk, tpos);
    u->multiplier = '*';
    _parse_interlace(ps, chunk, tpos, u);
  }
  return true;
}

static bool _parse_branch_line(ParseState* ps, TokenChunk* chunk, int32_t* tpos, PegUnit* branches);

// branches = @nl* branch_line+<@nl> @nl*
static bool _parse_branches(ParseState* ps, TokenChunk* chunk, PegUnit* u) {
  int32_t tpos = 0;
  u->kind = PEG_BRANCHES;
  _skip_nl(chunk, &tpos);
  while (!_at_end(chunk, tpos)) {
    if (!_parse_branch_line(ps, chunk, &tpos, u)) {
      return false;
    }
    _skip_nl(chunk, &tpos);
  }
  return true;
}

// peg_unit = [ @peg_id multiplier? | @peg_tok_id multiplier? | peg_str multiplier? | branches ]
static bool _parse_peg_unit(ParseState* ps, TokenChunk* chunk, int32_t* tpos, PegUnit* u) {
  Token* t = _peek(chunk, *tpos);
  if (!t) {
    return true;
  }
  if (t->tok_id == TOK_PEG_ID) {
    _next(chunk, tpos);
    char* name = _tok_str(ps, t);
    u->kind = PEG_CALL;
    u->id = symtab_intern(&ps->rule_names, name);
    free(name);
    return _parse_multiplier(ps, chunk, tpos, u);
  }
  if (t->tok_id == TOK_PEG_TOK_ID) {
    _next(chunk, tpos);
    char* name = _tok_str_skip(ps, t, 1);
    u->kind = PEG_TOK;
    u->id = symtab_intern(&ps->tokens, name);
    free(name);
    return _parse_multiplier(ps, chunk, tpos, u);
  }
  if (t->tok_id == SCOPE_PEG_STR) {
    _next(chunk, tpos);
    TokenChunk* sc = _scope_chunk(ps, t);
    u->kind = PEG_TOK;
    u->id = symtab_intern(&ps->tokens, sc->value);
    free(sc->value);
    sc->value = NULL;
    return _parse_multiplier(ps, chunk, tpos, u);
  }
  if (t->tok_id == SCOPE_BRANCHES) {
    _next(chunk, tpos);
    return _parse_branches(ps, _scope_chunk(ps, t), u);
  }
  return true;
}

// seq = peg_unit+
static bool _parse_seq(ParseState* ps, TokenChunk* chunk, int32_t* tpos, PegUnit* seq) {
  while (!_at_end(chunk, *tpos) && _is_peg_unit(_peek(chunk, *tpos)->tok_id)) {
    if (!seq->children) {
      seq->children = darray_new(sizeof(PegUnit), 0);
    }
    darray_push(seq->children, ((PegUnit){0}));
    if (!_parse_peg_unit(ps, chunk, tpos, &seq->children[darray_size(seq->children) - 1])) {
      return false;
    }
  }
  return true;
}

// branch_line = [ peg_simple_unit @tag_id? | @tag_id ]
static bool _parse_branch_line(ParseState* ps, TokenChunk* chunk, int32_t* tpos, PegUnit* br) {
  if (!br->children) {
    br->children = darray_new(sizeof(PegUnit), 0);
  }
  if (_at(chunk, *tpos, TOK_TAG_ID)) {
    Token* t = _next(chunk, tpos);
    darray_push(br->children, ((PegUnit){.kind = PEG_SEQ, .tag = _tok_str(ps, t)}));
    return true;
  }
  darray_push(br->children, ((PegUnit){.kind = PEG_SEQ}));
  PegUnit* b = &br->children[darray_size(br->children) - 1];
  if (!_parse_seq(ps, chunk, tpos, b)) {
    return false;
  }
  if (_at(chunk, *tpos, TOK_TAG_ID)) {
    b->tag = _tok_str(ps, _next(chunk, tpos));
  } else if (_at(chunk, *tpos, SCOPE_PEG_TAG)) {
    Token* sc = _next(chunk, tpos);
    TokenChunk* tag_chunk = _scope_chunk(ps, sc);
    for (int32_t i = 0; i < (int32_t)darray_size(tag_chunk->tokens); i++) {
      if (tag_chunk->tokens[i].tok_id == TOK_TAG_ID) {
        b->tag = _tok_str(ps, &tag_chunk->tokens[i]);
        break;
      }
    }
  }
  return true;
}

// peg_rule = @peg_id @peg_assign seq
static bool _parse_peg_rule(ParseState* ps, TokenChunk* chunk, int32_t* tpos) {
  Token* name_tok = _expect(ps, chunk, tpos, TOK_PEG_ID, "peg rule name");
  if (!name_tok || !_expect(ps, chunk, tpos, LIT_EQ, "'='")) {
    return false;
  }
  if (!ps->peg_rules) {
    ps->peg_rules = darray_new(sizeof(PegRule), 0);
  }
  char* name = _tok_str(ps, name_tok);
  int32_t gid = symtab_intern(&ps->rule_names, name);
  free(name);
  darray_push(ps->peg_rules, ((PegRule){.global_id = gid, .scope_id = -1, .seq = {.kind = PEG_SEQ}}));
  return _parse_seq(ps, chunk, tpos, &ps->peg_rules[darray_size(ps->peg_rules) - 1].seq);
}

// peg = @nl* peg_rule+<@nl> @nl*
static bool _parse_peg(ParseState* ps, TokenChunk* chunk, int32_t* tpos) {
  _skip_nl(chunk, tpos);
  while (!_at_end(chunk, *tpos)) {
    if (!_parse_peg_rule(ps, chunk, tpos)) {
      return false;
    }
    _skip_nl(chunk, tpos);
  }
  return true;
}

// ============================================================================
// Free / lifecycle
// ============================================================================

static void _free_vpa_unit(VpaUnit* u) {
  re_ir_free(u->re);
  darray_del(u->action_units);
  free(u->macro_name);
}

static void _free_peg_unit(PegUnit* u) {
  free(u->tag);
  for (int32_t i = 0; i < (int32_t)darray_size(u->children); i++) {
    _free_peg_unit(&u->children[i]);
  }
  darray_del(u->children);
}

static void _free_vpa_unit(VpaUnit* u);

static void _free_state(ParseState* ps) {
  tt_tree_del(ps->tree);
  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_scopes); i++) {
    free(ps->vpa_scopes[i].name);
    _free_vpa_unit(&ps->vpa_scopes[i].leader);
    for (int32_t j = 0; j < (int32_t)darray_size(ps->vpa_scopes[i].children); j++) {
      _free_vpa_unit(&ps->vpa_scopes[i].children[j]);
    }
    darray_del(ps->vpa_scopes[i].children);
  }
  darray_del(ps->vpa_scopes);
  for (int32_t i = 0; i < (int32_t)darray_size(ps->peg_rules); i++) {
    _free_peg_unit(&ps->peg_rules[i].seq);
  }
  darray_del(ps->peg_rules);
  for (int32_t i = 0; i < (int32_t)darray_size(ps->re_frags); i++) {
    free(ps->re_frags[i].name);
    re_ir_free(ps->re_frags[i].re);
  }
  darray_del(ps->re_frags);
  for (int32_t i = 0; i < (int32_t)darray_size(ps->effect_decls); i++) {
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
  ParseState* s = calloc(1, sizeof(ParseState));
  return s;
}

void parse_state_del(ParseState* ps) {
  if (!ps) {
    return;
  }
  _free_state(ps);
  free(ps);
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

  // init symtabs for VPA token/hook numbering
  symtab_init(&ps->tokens, 1);
  symtab_init(&ps->hooks, 0);
  symtab_intern(&ps->hooks, ".begin");   // HOOK_ID_BEGIN = 0
  symtab_intern(&ps->hooks, ".end");     // HOOK_ID_END = 1
  symtab_intern(&ps->hooks, ".fail");    // HOOK_ID_FAIL = 2
  symtab_intern(&ps->hooks, ".unparse"); // HOOK_ID_UNPARSE = 3
  symtab_init(&ps->scope_names, 0);
  symtab_init(&ps->rule_names, 0);

  UstrIter it = {0};
  ustr_iter_init(&it, src, 0);
  LexCtx lctx = {.ps = ps, .tree = ps->tree, .cp_count = ps->src_len, .it = it};
  ps->shared = &lctx.shared;
  _lex_scope(&lctx, SCOPE_MAIN);

  int32_t vpa_idx = -1, peg_idx = -1;
  for (int32_t i = 0; i < (int32_t)darray_size(ps->tree->table); i++) {
    if (ps->tree->table[i].scope_id == SCOPE_VPA && vpa_idx < 0) {
      vpa_idx = i;
    } else if (ps->tree->table[i].scope_id == SCOPE_PEG && peg_idx < 0) {
      peg_idx = i;
    }
  }
  if (vpa_idx < 0) {
    parse_error(ps, "missing [[vpa]]");
    return false;
  }
  if (peg_idx < 0) {
    parse_error(ps, "missing [[peg]]");
    return false;
  }

  // first pass: collect %define fragments
  TokenChunk* vpa_chunk = &ps->tree->table[vpa_idx];
  int32_t tpos = 0;
  {
    int32_t saved = tpos;
    while (!_at_end(vpa_chunk, tpos)) {
      if (_at(vpa_chunk, tpos, LIT_DEFINE)) {
        if (!_parse_define_frag(ps, vpa_chunk, &tpos)) {
          return false;
        }
      } else {
        _next(vpa_chunk, &tpos);
      }
    }
    tpos = saved;
  }

  if (!_parse_vpa(ps, vpa_chunk, &tpos)) {
    return false;
  }

  TokenChunk* peg_chunk = &ps->tree->table[peg_idx];
  tpos = 0;
  if (!_parse_peg(ps, peg_chunk, &tpos)) {
    return false;
  }

  return true;
}
