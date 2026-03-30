// specs/parse.md
#include "parse.h"
#include "darray.h"
#include "peg.h"
#include "re.h"
#include "re_ir.h"
#include "token_chunk.h"
#include "ustr.h"
#include "vpa.h"

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Generated DFA lexer functions (linked from compiled nest_lex.ll) ---

typedef struct {
  int64_t state;
  int64_t action;
} LexResult;

extern LexResult lex_main(int64_t state, int64_t cp);
extern LexResult lex_vpa(int64_t state, int64_t cp);
extern LexResult lex_peg(int64_t state, int64_t cp);
extern LexResult lex_re(int64_t state, int64_t cp);
extern LexResult lex_re_ref(int64_t state, int64_t cp);
extern LexResult lex_re_str(int64_t state, int64_t cp);
extern LexResult lex_charclass(int64_t state, int64_t cp);
extern LexResult lex_keyword_str(int64_t state, int64_t cp);

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
    Location loc = tc_locate(ps->tree, t->cp_start);
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

// --- String allocation helpers ---

static char* _tok_strdup(ParseState* ps, Token* t) {
  char* u = ustr_slice(ps->src, t->cp_start, t->cp_start + t->cp_size);
  char* s = strdup(u);
  ustr_del(u);
  return s;
}

static char* _tok_strdup_skip(ParseState* ps, Token* t, int32_t skip) {
  char* u = ustr_slice(ps->src, t->cp_start + skip, t->cp_start + t->cp_size);
  char* s = strdup(u);
  ustr_del(u);
  return s;
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

// --- Build string span from a chunk's tokens ---

static StrSpan _build_str_span(TokenChunk* chunk) {
  int32_t n = (int32_t)darray_size(chunk->tokens);
  if (n == 0) {
    return (StrSpan){0, 0};
  }
  Token* first = &chunk->tokens[0];
  Token* last = &chunk->tokens[n - 1];
  int32_t start = first->cp_start;
  int32_t end = last->cp_start + last->cp_size;
  return (StrSpan){start, end - start};
}

// --- Pushdown automaton lexer ---

#define TOK_RE_AST_BASE 20000
#define TOK_STR_SPAN_BASE 30000

typedef struct {
  ParseState* ps;
  TokenTree* tree;
  int32_t cp_count;
  UstrIter it;
  int32_t last_quote_cp;
} LexCtx;

typedef struct {
  ScopeId id;
  LexFunc lex_fn;
} ScopeConfig;

static int32_t _next_cp(LexCtx* ctx) {
  if (ctx->it.cp_idx >= ctx->cp_count) {
    return -2;
  }
  return ustr_iter_next(&ctx->it);
}

static void _lex_scope(LexCtx* ctx, ScopeId scope_id);

// --- Decode a codepoint from a token span ---

static int32_t _decode_codepoint(const char* src, Token* t) {
  char* s = ustr_slice(src, t->cp_start, t->cp_start + t->cp_size);
  int32_t len = t->cp_size;
  int32_t result;
  if (t->tok_id == TOK_CHAR) {
    UstrIter it = {0};
    ustr_iter_init(&it, s, len);
    result = ustr_iter_next(&it);
    ustr_del(s);
    return result;
  }
  if (t->tok_id == TOK_CODEPOINT) {
    // \u{XX..} — all ASCII, byte offsets == codepoint offsets
    result = re_hex_to_codepoint(s + 2, (size_t)(len - 3));
    ustr_del(s);
    return result;
  }
  if (t->tok_id == TOK_C_ESCAPE) {
    // \n, \t etc — always ASCII
    if (len >= 2) {
      int32_t esc = re_c_escape(s[1]);
      if (esc >= 0) {
        ustr_del(s);
        return esc;
      }
      result = s[1];
      ustr_del(s);
      return result;
    }
    result = s[0];
    ustr_del(s);
    return result;
  }
  if (t->tok_id == TOK_PLAIN_ESCAPE) {
    // \X — skip the '\' (1 ASCII byte), decode the next codepoint
    if (len >= 2) {
      UstrIter it = {0};
      ustr_iter_init(&it, s + 1, len - 1);
      result = ustr_iter_next(&it);
      ustr_del(s);
      return result;
    }
    result = s[0];
    ustr_del(s);
    return result;
  }
  UstrIter it = {0};
  ustr_iter_init(&it, s, len);
  result = ustr_iter_next(&it);
  ustr_del(s);
  return result;
}

// --- ReIr parsing from token chunk (recursive descent) ---

typedef struct {
  ParseState* ps;
  Token* tokens;
  int32_t count;
  int32_t pos;
  bool icase;
} ReParseCtx;

static Token* _re_peek(ReParseCtx* rctx) {
  if (rctx->pos < rctx->count) {
    return &rctx->tokens[rctx->pos];
  }
  return NULL;
}

static Token* _re_next(ReParseCtx* rctx) {
  Token* t = _re_peek(rctx);
  if (t) {
    rctx->pos++;
  }
  return t;
}

static bool _re_at(ReParseCtx* rctx, int32_t id) {
  Token* t = _re_peek(rctx);
  return t && t->tok_id == id;
}

static void _parse_re_expr(ReParseCtx* rctx, ReIr* ir);

static void _parse_charclass(ReParseCtx* rctx, ReIr* ir) {
  Token* begin = _re_next(rctx);
  if (!begin) {
    return;
  }
  re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);

  char* text = ustr_slice(rctx->ps->src, begin->cp_start, begin->cp_start + begin->cp_size);
  bool neg = false;
  for (const char* p = text; *p; p++) {
    if (*p == '^') {
      neg = true;
      break;
    }
  }
  ustr_del(text);
  if (neg) {
    re_ir_emit(ir, RE_IR_RANGE_NEG, 0, 0);
  }

  while (rctx->pos < rctx->count) {
    Token* t = _re_peek(rctx);
    if (!t) {
      break;
    }
    if ((t->tok_id == TOK_CHAR || t->tok_id == TOK_CODEPOINT || t->tok_id == TOK_C_ESCAPE ||
         t->tok_id == TOK_PLAIN_ESCAPE) &&
        rctx->pos + 2 < rctx->count && rctx->tokens[rctx->pos + 1].tok_id == TOK_RANGE_SEP) {
      int32_t lo = _decode_codepoint(rctx->ps->src, t);
      rctx->pos += 2;
      Token* hi_t = _re_next(rctx);
      int32_t hi = hi_t ? _decode_codepoint(rctx->ps->src, hi_t) : lo;
      re_ir_emit(ir, RE_IR_APPEND_CH, lo, hi);
    } else if (t->tok_id == TOK_CHAR || t->tok_id == TOK_CODEPOINT || t->tok_id == TOK_C_ESCAPE ||
               t->tok_id == TOK_PLAIN_ESCAPE) {
      int32_t cp = _decode_codepoint(rctx->ps->src, t);
      _re_next(rctx);
      re_ir_emit(ir, RE_IR_APPEND_CH, cp, cp);
    } else {
      break;
    }
  }

  if (rctx->icase) {
    re_ir_emit(ir, RE_IR_RANGE_IC, 0, 0);
  }
  re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
}

static void _emit_shorthand_class(ReIr* ir, int32_t tok_id) {
  switch (tok_id) {
  case TOK_RE_DOT:
    re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
    re_ir_emit(ir, RE_IR_RANGE_NEG, 0, 0);
    re_ir_emit(ir, RE_IR_APPEND_CH, '\n', '\n');
    re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
    break;
  case TOK_RE_SPACE_CLASS:
    re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
    re_ir_emit(ir, RE_IR_APPEND_GROUP_S, 0, 0);
    re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
    break;
  case TOK_RE_WORD_CLASS:
    re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
    re_ir_emit(ir, RE_IR_APPEND_GROUP_W, 0, 0);
    re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
    break;
  case TOK_RE_DIGIT_CLASS:
    re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
    re_ir_emit(ir, RE_IR_APPEND_GROUP_D, 0, 0);
    re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
    break;
  case TOK_RE_HEX_CLASS:
    re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
    re_ir_emit(ir, RE_IR_APPEND_GROUP_H, 0, 0);
    re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
    break;
  default:
    break;
  }
}

static void _parse_re_unit(ReParseCtx* rctx, ReIr* ir) {
  Token* t = _re_peek(rctx);
  if (!t) {
    return;
  }

  switch (t->tok_id) {
  case TOK_RE_OPS_LPAREN:
    _re_next(rctx);
    re_ir_emit(ir, RE_IR_LPAREN, 0, 0);
    _parse_re_expr(rctx, ir);
    if (_re_at(rctx, TOK_RE_OPS_RPAREN)) {
      _re_next(rctx);
    }
    re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
    break;

  case TOK_CHARCLASS_BEGIN:
    _parse_charclass(rctx, ir);
    break;

  case TOK_RE_DOT:
  case TOK_RE_SPACE_CLASS:
  case TOK_RE_WORD_CLASS:
  case TOK_RE_DIGIT_CLASS:
  case TOK_RE_HEX_CLASS:
    _re_next(rctx);
    _emit_shorthand_class(ir, t->tok_id);
    break;

  case TOK_RE_BOF:
    _re_next(rctx);
    re_ir_emit_ch(ir, LEX_CP_BOF);
    break;

  case TOK_RE_EOF:
    _re_next(rctx);
    re_ir_emit_ch(ir, LEX_CP_EOF);
    break;

  case TOK_RE_REF: {
    _re_next(rctx);
    break;
  }

  case TOK_CHAR:
  case TOK_CODEPOINT:
  case TOK_C_ESCAPE:
  case TOK_PLAIN_ESCAPE: {
    _re_next(rctx);
    int32_t cp = _decode_codepoint(rctx->ps->src, t);
    re_ir_emit(ir, rctx->icase ? RE_IR_APPEND_CH_IC : RE_IR_APPEND_CH, cp, cp);
    break;
  }

  default:
    return;
  }
}

static bool _is_re_unit_start(int32_t id) {
  return id == TOK_RE_OPS_LPAREN || id == TOK_CHARCLASS_BEGIN || id == TOK_RE_DOT || id == TOK_RE_SPACE_CLASS ||
         id == TOK_RE_WORD_CLASS || id == TOK_RE_DIGIT_CLASS || id == TOK_RE_HEX_CLASS || id == TOK_RE_BOF ||
         id == TOK_RE_EOF || id == TOK_RE_REF || id == TOK_CHAR || id == TOK_CODEPOINT || id == TOK_C_ESCAPE ||
         id == TOK_PLAIN_ESCAPE;
}

static void _parse_re_quantified(ReParseCtx* rctx, ReIr* ir) {
  int32_t unit_start = (int32_t)darray_size(*ir);
  _parse_re_unit(rctx, ir);
  int32_t unit_end = (int32_t)darray_size(*ir);
  if (unit_start == unit_end) {
    return;
  }

  Token* q = _re_peek(rctx);
  if (!q) {
    return;
  }

  if (q->tok_id == TOK_RE_OPS_MAYBE) {
    _re_next(rctx);
    ReIrOp lparen = {RE_IR_LPAREN, 0, 0};
    *ir = darray_grow(*ir, darray_size(*ir) + 3);
    memmove(&(*ir)[unit_start + 1], &(*ir)[unit_start], (size_t)(unit_end - unit_start) * sizeof(ReIrOp));
    (*ir)[unit_start] = lparen;
    re_ir_emit(ir, RE_FORK, 0, 0);
    re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
  } else if (q->tok_id == TOK_RE_OPS_PLUS) {
    _re_next(rctx);
    re_ir_emit(ir, RE_IR_LPAREN, 0, 0);
    for (int32_t i = unit_start; i < unit_end; i++) {
      darray_push(*ir, (*ir)[i]);
    }
    re_ir_emit(ir, RE_FORK, 0, 0);
    re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
  } else if (q->tok_id == TOK_RE_OPS_STAR) {
    _re_next(rctx);
    ReIrOp lparen = {RE_IR_LPAREN, 0, 0};
    *ir = darray_grow(*ir, darray_size(*ir) + 3);
    memmove(&(*ir)[unit_start + 1], &(*ir)[unit_start], (size_t)(unit_end - unit_start) * sizeof(ReIrOp));
    (*ir)[unit_start] = lparen;
    for (int32_t i = unit_start + 1; i <= unit_end; i++) {
      darray_push(*ir, (*ir)[i]);
    }
    re_ir_emit(ir, RE_FORK, 0, 0);
    re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
  }
}

static void _parse_re_expr(ReParseCtx* rctx, ReIr* ir) {
  while (rctx->pos < rctx->count && _is_re_unit_start(_re_peek(rctx)->tok_id)) {
    _parse_re_quantified(rctx, ir);
  }

  while (_re_at(rctx, TOK_RE_OPS_ALT)) {
    _re_next(rctx);
    re_ir_emit(ir, RE_FORK, 0, 0);
    while (rctx->pos < rctx->count) {
      Token* t = _re_peek(rctx);
      if (!t || !_is_re_unit_start(t->tok_id)) {
        break;
      }
      _parse_re_quantified(rctx, ir);
    }
  }
}

static ReIr _parse_re_tokens(ParseState* ps, Token* tokens, int32_t count, bool icase) {
  ReParseCtx rctx = {.ps = ps, .tokens = tokens, .count = count, .pos = 0, .icase = icase};
  ReIr ir = re_ir_new();
  _parse_re_expr(&rctx, &ir);
  return ir;
}

// --- Scope-specific lex handlers ---

static void _handle_re_scope(LexCtx* ctx, int32_t tag_start, int32_t tag_size) {
  // push a child chunk for the re scope, lex into it
  TokenChunk* parent = ctx->tree->current;
  _lex_scope(ctx, SCOPE_RE);
  // the child chunk was pushed/popped by _lex_scope; it's the last entry in the table
  int32_t child_idx = (int32_t)darray_size(ctx->tree->table) - 1;
  TokenChunk* child = &ctx->tree->table[child_idx];

  // parse mode flags from the re_tag token text
  bool icase = false;
  char* tag_text = ustr_slice(ctx->ps->src, tag_start, tag_start + tag_size);
  for (const char* p = tag_text; *p; p++) {
    if (*p == '/') {
      break;
    }
    if (*p == 'i') {
      icase = true;
    }
  }
  ustr_del(tag_text);

  int32_t count = (int32_t)darray_size(child->tokens);
  ReIr ir = _parse_re_tokens(ctx->ps, child->tokens, count, icase);

  if (!ctx->ps->re_irs) {
    ctx->ps->re_irs = darray_new(sizeof(ReIr), 0);
  }
  int32_t idx = (int32_t)darray_size(ctx->ps->re_irs);
  darray_push(ctx->ps->re_irs, ir);

  tc_add(parent, (Token){.tok_id = TOK_RE_AST_BASE + idx, .cp_start = tag_start, .cp_size = tag_size});
}

static void _handle_str_scope(LexCtx* ctx, ScopeId scope_id, int32_t tok_cp_start, int32_t tok_id_base) {
  TokenChunk* parent = ctx->tree->current;
  _lex_scope(ctx, scope_id);
  int32_t child_idx = (int32_t)darray_size(ctx->tree->table) - 1;
  TokenChunk* child = &ctx->tree->table[child_idx];

  StrSpan span = _build_str_span(child);

  if (!ctx->ps->str_spans) {
    ctx->ps->str_spans = darray_new(sizeof(StrSpan), 0);
  }
  int32_t idx = (int32_t)darray_size(ctx->ps->str_spans);
  darray_push(ctx->ps->str_spans, span);

  int32_t cp_size = ctx->it.cp_idx - tok_cp_start;
  tc_add(parent, (Token){.tok_id = tok_id_base + idx, .cp_start = tok_cp_start, .cp_size = cp_size});
}

// --- Pushdown automaton lexer ---

static void _lex_scope(LexCtx* ctx, ScopeId scope_id) {
  static const ScopeConfig configs[] = {
      [SCOPE_MAIN] = {SCOPE_MAIN, lex_main},
      [SCOPE_VPA] = {SCOPE_VPA, lex_vpa},
      [SCOPE_PEG] = {SCOPE_PEG, lex_peg},
      [SCOPE_RE] = {SCOPE_RE, lex_re},
      [SCOPE_RE_REF] = {SCOPE_RE_REF, lex_re_ref},
      [SCOPE_RE_STR] = {SCOPE_RE_STR, lex_re_str},
      [SCOPE_CHARCLASS] = {SCOPE_CHARCLASS, lex_charclass},
      [SCOPE_KEYWORD_STR] = {SCOPE_KEYWORD_STR, lex_keyword_str},
  };
  ScopeConfig cfg = configs[scope_id];

  TokenChunk* chunk = tc_push(ctx->tree);
  chunk->scope_id = scope_id;

  int64_t state = 0;
  int32_t last_action = 0;
  int32_t tok_cp_start = ctx->it.cp_idx;

  while (ctx->it.cp_idx < ctx->cp_count || last_action != 0) {
    int32_t cp_cp_idx = ctx->it.cp_idx;
    int32_t cp = _next_cp(ctx);

    LexResult r = cfg.lex_fn(state, cp);

    if (r.action == LEX_ACTION_NOMATCH) {
      if (last_action == TOK_END || last_action == TOK_UNPARSE_END) {
        tc_pop(ctx->tree);
        return;
      } else if (last_action == TOK_IGNORE) {
        break;
      } else if (last_action <= SCOPE_COUNT) {
        if (last_action == SCOPE_KEYWORD_STR || last_action == SCOPE_RE_STR) {
          ctx->last_quote_cp = tok_cp_start;
        }
        if (last_action == SCOPE_RE) {
          _handle_re_scope(ctx, tok_cp_start, cp_cp_idx - tok_cp_start);
        } else if (last_action == SCOPE_RE_STR) {
          _handle_str_scope(ctx, SCOPE_RE_STR, tok_cp_start, TOK_STR_SPAN_BASE);
        } else if (last_action == SCOPE_KEYWORD_STR) {
          _handle_str_scope(ctx, SCOPE_KEYWORD_STR, tok_cp_start, TOK_STR_SPAN_BASE);
        } else {
          _lex_scope(ctx, last_action);
        }
      } else {
        tc_add(chunk, (Token){.tok_id = last_action, .cp_start = tok_cp_start, .cp_size = cp_cp_idx - tok_cp_start});
      }

      if (cp == LEX_CP_EOF) {
        tc_pop(ctx->tree);
        return;
      }

      tok_cp_start = cp_cp_idx;
      last_action = 0;
      state = 0;
    } else {
      state = r.state;
      last_action = r.action;
    }
  }
  tc_pop(ctx->tree);
}

// --- Token cursor helpers (operate on ps->read_chunk) ---

static int32_t _chunk_size(ParseState* ps) { return (int32_t)darray_size(ps->read_chunk->tokens); }

static Token* _peek(ParseState* ps) {
  if (ps->tpos < _chunk_size(ps)) {
    return &ps->read_chunk->tokens[ps->tpos];
  }
  return NULL;
}

static Token* _next(ParseState* ps) {
  Token* t = _peek(ps);
  if (t) {
    ps->tpos++;
  }
  return t;
}

static void _skip_nl(ParseState* ps) {
  while (ps->tpos < _chunk_size(ps) && ps->read_chunk->tokens[ps->tpos].tok_id == TOK_NL) {
    ps->tpos++;
  }
}

static bool _at_end(ParseState* ps) { return ps->tpos >= _chunk_size(ps); }

static bool _at(ParseState* ps, int32_t id) {
  Token* t = _peek(ps);
  return t && t->tok_id == id;
}

// --- Shared VPA followup parsing ---

static bool _parse_vpa_scope_body(ParseState* ps, VpaRule* rule);

static void _parse_unit_followups(ParseState* ps, VpaUnit* unit, VpaRule* rule) {
  for (;;) {
    Token* ft = _peek(ps);
    if (!ft || ft->tok_id == TOK_NL) {
      break;
    }
    if (ft->tok_id == TOK_TOK_ID) {
      _next(ps);
      parse_set_str(&unit->name, _tok_strdup_skip(ps, ft, 1));
    } else if (ft->tok_id == TOK_HOOK_BEGIN || ft->tok_id == TOK_HOOK_END || ft->tok_id == TOK_HOOK_FAIL ||
               ft->tok_id == TOK_HOOK_UNPARSE) {
      _next(ps);
      unit->hook = ft->tok_id;
    } else if (ft->tok_id == TOK_USER_HOOK_ID) {
      _next(ps);
      parse_set_str(&unit->user_hook, _tok_strdup(ps, ft));
    } else if (ft->tok_id == TOK_SCOPE_BEGIN) {
      _next(ps);
      unit->kind = VPA_SCOPE;
      VpaRule scope_rule = {.name = strdup(rule->name), .is_scope = true};
      _parse_vpa_scope_body(ps, &scope_rule);
      unit->children = scope_rule.units;
      free(scope_rule.name);
    } else {
      break;
    }
  }
}

// --- VPA rule body: bool-returning try-functions ---

static void _add_vpa_unit(VpaRule* rule, VpaUnit unit) {
  if (!rule->units) {
    rule->units = darray_new(sizeof(VpaUnit), 0);
  }
  darray_push(rule->units, unit);
}

static bool _parse_vpa_regexp(ParseState* ps, VpaRule* rule) {
  Token* t = _peek(ps);
  if (!t || t->tok_id < TOK_RE_AST_BASE || t->tok_id >= TOK_RE_AST_BASE + (int32_t)darray_size(ps->re_irs)) {
    return false;
  }
  _next(ps);
  VpaUnit unit = {.kind = VPA_REGEXP};

  int32_t ir_idx = t->tok_id - TOK_RE_AST_BASE;
  unit.re = ps->re_irs[ir_idx];
  ps->re_irs[ir_idx] = NULL;

  unit.binary_mode = false;
  char* tag_text = ustr_slice(ps->src, t->cp_start, t->cp_start + t->cp_size);
  for (const char* p = tag_text; *p; p++) {
    if (*p == '/') {
      break;
    }
    if (*p == 'b') {
      unit.binary_mode = true;
    }
  }
  ustr_del(tag_text);

  _parse_unit_followups(ps, &unit, rule);
  _add_vpa_unit(rule, unit);
  return true;
}

static bool _parse_vpa_re_str(ParseState* ps, VpaRule* rule) {
  Token* t = _peek(ps);
  if (!t || t->tok_id < TOK_STR_SPAN_BASE || t->tok_id >= TOK_STR_SPAN_BASE + (int32_t)darray_size(ps->str_spans)) {
    return false;
  }
  _next(ps);
  VpaUnit unit = {.kind = VPA_REGEXP};
  StrSpan span = ps->str_spans[t->tok_id - TOK_STR_SPAN_BASE];
  unit.re = re_ir_build_literal(ps->src, span.off, span.len);

  _parse_unit_followups(ps, &unit, rule);
  _add_vpa_unit(rule, unit);
  return true;
}

static bool _parse_vpa_ref(ParseState* ps, VpaRule* rule) {
  Token* t = _peek(ps);
  if (!t || t->tok_id != TOK_VPA_ID) {
    return false;
  }
  _next(ps);
  VpaUnit unit = {.kind = VPA_REF, .name = _tok_strdup(ps, t)};
  _parse_unit_followups(ps, &unit, rule);
  _add_vpa_unit(rule, unit);
  return true;
}

static bool _parse_vpa_state_ref(ParseState* ps, VpaRule* rule) {
  Token* t = _peek(ps);
  if (!t || t->tok_id != TOK_STATE_ID) {
    return false;
  }
  _next(ps);
  VpaUnit unit = {.kind = VPA_STATE, .state_name = _tok_strdup_skip(ps, t, 1)};
  _parse_unit_followups(ps, &unit, rule);
  _add_vpa_unit(rule, unit);
  return true;
}

static bool _parse_vpa_macro_ref(ParseState* ps, VpaRule* rule) {
  Token* t = _peek(ps);
  if (!t || t->tok_id != TOK_MACRO_ID) {
    return false;
  }
  _next(ps);
  char* base = _tok_strdup_skip(ps, t, 1);
  VpaUnit unit = {.kind = VPA_REF, .name = parse_sfmt("*%s", base)};
  free(base);
  _add_vpa_unit(rule, unit);
  return true;
}

static bool _parse_vpa_pipe(ParseState* ps) {
  if (!_at(ps, TOK_OPS_PIPE)) {
    return false;
  }
  _next(ps);
  return true;
}

static bool _parse_vpa_nl(ParseState* ps) {
  if (!_at(ps, TOK_NL)) {
    return false;
  }
  ps->tpos++;
  return true;
}

static bool _parse_vpa_rule_body(ParseState* ps, VpaRule* rule) {
  if (_parse_vpa_regexp(ps, rule) || _parse_vpa_re_str(ps, rule) || _parse_vpa_state_ref(ps, rule) ||
      _parse_vpa_ref(ps, rule) || _parse_vpa_macro_ref(ps, rule) || _parse_vpa_pipe(ps) || _parse_vpa_nl(ps)) {
    return true;
  }
  _error_at(ps, _peek(ps), "unexpected token in rule body");
  return false;
}

static bool _parse_vpa_scope_body(ParseState* ps, VpaRule* rule) {
  _skip_nl(ps);
  while (!_at_end(ps) && !_at(ps, TOK_SCOPE_END)) {
    if (!_parse_vpa_rule_body(ps, rule)) {
      return false;
    }
  }
  Token* close = _peek(ps);
  if (!close || close->tok_id != TOK_SCOPE_END) {
    _error_at(ps, close, "expected '}'");
    return false;
  }
  _next(ps);
  return true;
}

static bool _parse_vpa_rule_line(ParseState* ps, VpaRule* rule) {
  while (!_at_end(ps) && !_at(ps, TOK_NL)) {
    if (!_parse_vpa_rule_body(ps, rule)) {
      return false;
    }
  }
  return true;
}

// --- VPA section: bool-returning top-level dispatchers ---

static bool _parse_vpa_rule(ParseState* ps) {
  if (!_at(ps, TOK_VPA_ID)) {
    return false;
  }
  Token* name_tok = _next(ps);
  if (!_at(ps, TOK_OPS_EQ)) {
    _error_at(ps, name_tok, "expected '=' after rule name");
    return false;
  }
  _next(ps);
  if (!ps->vpa_rules) {
    ps->vpa_rules = darray_new(sizeof(VpaRule), 0);
  }
  darray_push(ps->vpa_rules, ((VpaRule){0}));
  VpaRule* rule = &ps->vpa_rules[darray_size(ps->vpa_rules) - 1];
  rule->name = _tok_strdup(ps, name_tok);

  if (_at(ps, TOK_SCOPE_BEGIN)) {
    _next(ps);
    rule->is_scope = true;
    return _parse_vpa_scope_body(ps, rule);
  }
  return _parse_vpa_rule_line(ps, rule);
}

static bool _parse_macro_rule(ParseState* ps) {
  if (!_at(ps, TOK_MACRO_ID)) {
    return false;
  }
  Token* name_tok = _next(ps);
  if (!_at(ps, TOK_OPS_EQ)) {
    _error_at(ps, name_tok, "expected '=' after macro name");
    return false;
  }
  _next(ps);
  if (!ps->vpa_rules) {
    ps->vpa_rules = darray_new(sizeof(VpaRule), 0);
  }
  darray_push(ps->vpa_rules, ((VpaRule){0}));
  VpaRule* rule = &ps->vpa_rules[darray_size(ps->vpa_rules) - 1];
  rule->name = _tok_strdup_skip(ps, name_tok, 1);
  rule->is_macro = true;

  if (_at(ps, TOK_SCOPE_BEGIN)) {
    _next(ps);
    rule->is_scope = true;
    return _parse_vpa_scope_body(ps, rule);
  }
  return _parse_vpa_rule_line(ps, rule);
}

static bool _parse_keyword_decl(ParseState* ps) {
  if (!_at(ps, TOK_DIRECTIVES_KEYWORD)) {
    return false;
  }
  Token* kw_tok = _next(ps);

  Token* group_tok = _next(ps);
  if (!group_tok || group_tok->tok_id != TOK_VPA_ID) {
    _error_at(ps, kw_tok, "expected keyword group name after %%keyword");
    return false;
  }
  char* group = _tok_strdup(ps, group_tok);

  while (!_at_end(ps) && !_at(ps, TOK_NL)) {
    Token* t = _peek(ps);
    if (t->tok_id < TOK_STR_SPAN_BASE) {
      break;
    }
    _next(ps);
    StrSpan span = ps->str_spans[t->tok_id - TOK_STR_SPAN_BASE];
    if (!ps->keywords) {
      ps->keywords = darray_new(sizeof(KeywordEntry), 0);
    }
    KeywordEntry kw = {.group = strdup(group), .lit_off = span.off, .lit_len = span.len, .src = ps->src};
    darray_push(ps->keywords, kw);
  }
  free(group);
  return true;
}

static bool _parse_ignore_decl(ParseState* ps) {
  if (!_at(ps, TOK_DIRECTIVES_IGNORE)) {
    return false;
  }
  _next(ps);
  while (!_at_end(ps) && !_at(ps, TOK_NL)) {
    Token* t = _peek(ps);
    if (t->tok_id != TOK_TOK_ID) {
      break;
    }
    _next(ps);
    if (!ps->ignores.names) {
      ps->ignores.names = darray_new(sizeof(char*), 0);
    }
    char* name = _tok_strdup_skip(ps, t, 1);
    darray_push(ps->ignores.names, name);
  }
  return true;
}

static bool _parse_state_decl(ParseState* ps) {
  if (!_at(ps, TOK_DIRECTIVES_STATE)) {
    return false;
  }
  Token* state_tok = _next(ps);
  Token* t = _next(ps);
  if (!t || t->tok_id != TOK_STATE_ID) {
    _error_at(ps, state_tok, "expected state name after %%state");
    return false;
  }
  if (!ps->states) {
    ps->states = darray_new(sizeof(StateDecl), 0);
  }
  StateDecl sd = {.name = _tok_strdup_skip(ps, t, 1)};
  darray_push(ps->states, sd);
  return true;
}

static bool _parse_effect_decl(ParseState* ps) {
  if (!_at(ps, TOK_DIRECTIVES_EFFECT)) {
    return false;
  }
  Token* effect_tok = _next(ps);

  Token* hook_tok = _next(ps);
  if (!hook_tok || hook_tok->tok_id != TOK_USER_HOOK_ID) {
    _error_at(ps, effect_tok, "expected hook name after %%effect");
    return false;
  }
  if (!_at(ps, TOK_OPS_EQ)) {
    _error_at(ps, hook_tok, "expected '=' in %%effect");
    return false;
  }
  _next(ps);

  if (!ps->effects) {
    ps->effects = darray_new(sizeof(EffectDecl), 0);
  }
  EffectDecl ed = {.hook_name = _tok_strdup(ps, hook_tok), .effects = darray_new(sizeof(int32_t), 0)};

  while (!_at_end(ps) && !_at(ps, TOK_NL)) {
    Token* t = _peek(ps);
    if (t->tok_id == TOK_OPS_PIPE) {
      _next(ps);
      continue;
    }
    if (t->tok_id == TOK_TOK_ID || t->tok_id == TOK_HOOK_BEGIN || t->tok_id == TOK_HOOK_END ||
        t->tok_id == TOK_HOOK_FAIL || t->tok_id == TOK_HOOK_UNPARSE) {
      _next(ps);
      darray_push(ed.effects, t->tok_id);
    } else {
      break;
    }
  }
  darray_push(ps->effects, ed);
  return true;
}

static bool _parse_vpa_section(ParseState* ps) {
  _skip_nl(ps);
  while (!_at_end(ps)) {
    Token* t = _peek(ps);
    if (!t) {
      break;
    }
    if (t->tok_id == TOK_NL) {
      ps->tpos++;
      continue;
    }
    if (t->tok_id == TOK_PEG_ID || t->tok_id == TOK_PEG_TOK_ID || t->tok_id == TOK_PEG_OPS_ASSIGN) {
      break;
    }
    if (_parse_keyword_decl(ps) || _parse_ignore_decl(ps) || _parse_state_decl(ps) || _parse_effect_decl(ps) ||
        _parse_macro_rule(ps) || _parse_vpa_rule(ps)) {
      if (parse_has_error(ps)) {
        return false;
      }
      continue;
    }
    if (parse_has_error(ps)) {
      return false;
    }
    _error_at(ps, t, "unexpected token in [[vpa]] section");
    return false;
  }
  return true;
}

// --- Recursive descent: PEG section ---

static bool _parse_peg_seq(ParseState* ps, PegUnit* seq);

static bool _parse_peg_unit(ParseState* ps, PegUnit* unit) {
  Token* t = _peek(ps);
  if (!t) {
    return true;
  }

  if (t->tok_id == TOK_PEG_ID) {
    _next(ps);
    unit->kind = PEG_ID;
    unit->name = _tok_strdup(ps, t);
  } else if (t->tok_id == TOK_PEG_TOK_ID) {
    _next(ps);
    unit->kind = PEG_TOK;
    unit->name = _tok_strdup_skip(ps, t, 1);
  } else if (t->tok_id >= TOK_STR_SPAN_BASE) {
    _next(ps);
    unit->kind = PEG_TOK;
    StrSpan span = ps->str_spans[t->tok_id - TOK_STR_SPAN_BASE];
    char* u = ustr_slice(ps->src, span.off, span.off + span.len);
    unit->name = strdup(u);
    ustr_del(u);
  } else if (t->tok_id == TOK_BRANCHES_BEGIN) {
    _next(ps);
    unit->kind = PEG_BRANCHES;
    _skip_nl(ps);
    while (!_at_end(ps) && !_at(ps, TOK_BRANCHES_END)) {
      if (_at(ps, TOK_NL)) {
        ps->tpos++;
        continue;
      }
      if (!unit->children) {
        unit->children = darray_new(sizeof(PegUnit), 0);
      }
      darray_push(unit->children, ((PegUnit){0}));
      PegUnit* branch = &unit->children[darray_size(unit->children) - 1];
      branch->kind = PEG_SEQ;
      if (!_parse_peg_seq(ps, branch)) {
        return false;
      }

      Token* tag_t = _peek(ps);
      if (tag_t && tag_t->tok_id == TOK_TAG_ID) {
        _next(ps);
        char* tag_text = _tok_strdup(ps, tag_t);
        char* p = tag_text;
        if (*p == ':') {
          p++;
        }
        while (*p == ' ') {
          p++;
        }
        branch->tag = strdup(p);
        free(tag_text);
      }
    }
    Token* close = _peek(ps);
    if (!close || close->tok_id != TOK_BRANCHES_END) {
      _error_at(ps, close ? close : t, "expected ']' to close branches");
      return false;
    }
    _next(ps);
    return true;
  } else {
    return true;
  }

  // Multipliers
  Token* mt = _peek(ps);
  if (mt) {
    if (mt->tok_id == TOK_PEG_OPS_QUESTION) {
      _next(ps);
      unit->multiplier = '?';
    } else if (mt->tok_id == TOK_PEG_OPS_PLUS) {
      _next(ps);
      unit->multiplier = '+';
    } else if (mt->tok_id == TOK_PEG_OPS_STAR) {
      _next(ps);
      unit->multiplier = '*';
    }

    if (unit->multiplier == '+' || unit->multiplier == '*') {
      if (_at(ps, TOK_PEG_OPS_LT)) {
        _next(ps);
        unit->interlace = calloc(1, sizeof(PegUnit));
        unit->interlace->kind = PEG_SEQ;
        if (!_parse_peg_seq(ps, unit->interlace)) {
          return false;
        }
        unit->ninterlace = 1;
        Token* gt = _peek(ps);
        if (!gt || gt->tok_id != TOK_PEG_OPS_GT) {
          _error_at(ps, gt, "expected '>' to close interlace");
          return false;
        }
        _next(ps);
      }
    }
  }
  return true;
}

static bool _is_peg_unit_start(int32_t id) {
  return id == TOK_PEG_ID || id == TOK_PEG_TOK_ID || id == TOK_BRANCHES_BEGIN || id >= TOK_STR_SPAN_BASE;
}

static bool _parse_peg_seq(ParseState* ps, PegUnit* seq) {
  while (!_at_end(ps)) {
    Token* t = _peek(ps);
    if (!t || !_is_peg_unit_start(t->tok_id)) {
      break;
    }
    if (!seq->children) {
      seq->children = darray_new(sizeof(PegUnit), 0);
    }
    darray_push(seq->children, ((PegUnit){0}));
    PegUnit* child = &seq->children[darray_size(seq->children) - 1];
    if (!_parse_peg_unit(ps, child)) {
      return false;
    }
  }
  return true;
}

static bool _parse_peg_rule(ParseState* ps) {
  Token* name_tok = _next(ps);
  if (!name_tok || name_tok->tok_id != TOK_PEG_ID) {
    _error_at(ps, name_tok, "expected peg rule name");
    return false;
  }
  if (!_at(ps, TOK_PEG_OPS_ASSIGN)) {
    _error_at(ps, name_tok, "expected '=' after peg rule name");
    return false;
  }
  _next(ps);
  if (!ps->peg_rules) {
    ps->peg_rules = darray_new(sizeof(PegRule), 0);
  }
  darray_push(ps->peg_rules, ((PegRule){0}));
  PegRule* rule = &ps->peg_rules[darray_size(ps->peg_rules) - 1];
  rule->name = _tok_strdup(ps, name_tok);
  rule->seq.kind = PEG_SEQ;
  return _parse_peg_seq(ps, &rule->seq);
}

static bool _parse_peg_section(ParseState* ps) {
  _skip_nl(ps);
  while (!_at_end(ps)) {
    if (_at(ps, TOK_NL)) {
      ps->tpos++;
      continue;
    }
    if (_at(ps, TOK_PEG_ID)) {
      if (!_parse_peg_rule(ps)) {
        return false;
      }
    } else {
      _error_at(ps, _peek(ps), "unexpected token in [[peg]] section");
      return false;
    }
  }
  return true;
}

// --- Free helpers ---

static void _free_vpa_unit(VpaUnit* unit) {
  re_ir_free(unit->re);
  free(unit->name);
  free(unit->state_name);
  free(unit->user_hook);
  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    _free_vpa_unit(&unit->children[i]);
  }
  darray_del(unit->children);
}

static void _free_peg_unit(PegUnit* unit) {
  free(unit->name);
  free(unit->tag);
  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    _free_peg_unit(&unit->children[i]);
  }
  darray_del(unit->children);
  if (unit->interlace) {
    _free_peg_unit(unit->interlace);
    free(unit->interlace);
  }
}

static void _free_state(ParseState* ps) {
  tc_tree_del(ps->tree);
  ps->tree = NULL;
  ps->read_chunk = NULL;
  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules); i++) {
    free(ps->vpa_rules[i].name);
    for (int32_t j = 0; j < (int32_t)darray_size(ps->vpa_rules[i].units); j++) {
      _free_vpa_unit(&ps->vpa_rules[i].units[j]);
    }
    darray_del(ps->vpa_rules[i].units);
  }
  darray_del(ps->vpa_rules);
  for (int32_t i = 0; i < (int32_t)darray_size(ps->peg_rules); i++) {
    free(ps->peg_rules[i].name);
    free(ps->peg_rules[i].scope);
    _free_peg_unit(&ps->peg_rules[i].seq);
  }
  darray_del(ps->peg_rules);
  for (int32_t i = 0; i < (int32_t)darray_size(ps->re_irs); i++) {
    re_ir_free(ps->re_irs[i]);
  }
  darray_del(ps->re_irs);
  darray_del(ps->str_spans);
  for (int32_t i = 0; i < (int32_t)darray_size(ps->keywords); i++) {
    free(ps->keywords[i].group);
  }
  darray_del(ps->keywords);
  for (int32_t i = 0; i < (int32_t)darray_size(ps->states); i++) {
    free(ps->states[i].name);
  }
  darray_del(ps->states);
  for (int32_t i = 0; i < (int32_t)darray_size(ps->effects); i++) {
    free(ps->effects[i].hook_name);
    darray_del(ps->effects[i].effects);
  }
  darray_del(ps->effects);
  for (int32_t i = 0; i < (int32_t)darray_size(ps->ignores.names); i++) {
    free(ps->ignores.names[i]);
  }
  darray_del(ps->ignores.names);
}

ParseState* parse_state_new(void) {
  ParseState* ps = calloc(1, sizeof(ParseState));
  return ps;
}

void parse_state_del(ParseState* ps) {
  if (!ps) {
    return;
  }
  _free_state(ps);
  free(ps);
}

const char* parse_get_error(ParseState* ps) {
  if (!ps) {
    return NULL;
  }
  return ps->error[0] ? ps->error : NULL;
}

// --- Public API ---

bool parse_nest(ParseState* ps, const char* src) {
  ps->src = src;
  ps->src_len = ustr_size(src);

  ps->tree = tc_tree_new(src);

  UstrIter it = {0};
  ustr_iter_init(&it, src, ps->src_len);
  LexCtx lex_ctx = {
      .ps = ps,
      .tree = ps->tree,
      .cp_count = ps->src_len,
      .it = it,
  };

  _lex_scope(&lex_ctx, SCOPE_MAIN);

  ps->read_chunk = ps->tree->root;
  ps->tpos = 0;
  if (!_parse_vpa_section(ps)) {
    return false;
  }
  if (!_parse_peg_section(ps)) {
    return false;
  }
  if (!_at_end(ps)) {
    _error_at(ps, _peek(ps), "unexpected token after end of input");
    return false;
  }

  return true;
}
