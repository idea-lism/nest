// Parser for .nest syntax files.
// Pushdown automaton lexer (calls generated DFA functions from parse_gen)
// + recursive descent for structure.
// Produces processed data, then invokes vpa_gen() and peg_gen().

#include "parse.h"
#include "darray.h"
#include "header_writer.h"
#include "irwriter.h"
#include "peg.h"
#include "re_ast.h"
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
extern LexResult lex_re(int64_t state, int64_t cp);
extern LexResult lex_charclass(int64_t state, int64_t cp);
extern LexResult lex_dquote_str(int64_t state, int64_t cp);
extern LexResult lex_squote_str(int64_t state, int64_t cp);
extern LexResult lex_peg(int64_t state, int64_t cp);

#define LEX_ACTION_NOMATCH (-2)

typedef LexResult (*LexFunc)(int64_t, int64_t);

// --- String span (content region within quotes) ---

typedef struct {
  int32_t off;
  int32_t len;
} StrSpan;

// --- Parser state ---

typedef struct {
  const char* src;
  int32_t src_len;
  char* ustr;

  TokenChunk main_chunk;
  int32_t tpos;

  ReAstNode** re_asts; // darray
  StrSpan* str_spans;  // darray

  VpaRule* vpa_rules;     // darray
  KeywordEntry* keywords; // darray
  IgnoreSet ignores;
  StateDecl* states;   // darray
  EffectDecl* effects; // darray
  PegRule* peg_rules;  // darray

  char error[512];
} ParseState;

// --- Error reporting ---

static bool _has_error(ParseState* ps) { return ps->error[0] != '\0'; }

static void _error_at(ParseState* ps, Token* t, const char* fmt, ...) {
  if (_has_error(ps)) {
    return;
  }
  int32_t off = 0;
  if (t) {
    off = snprintf(ps->error, sizeof(ps->error), "%d:%d: ", t->line + 1, t->col + 1);
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ps->error + off, sizeof(ps->error) - (size_t)off, fmt, ap);
  va_end(ap);
}

static void _error(ParseState* ps, const char* fmt, ...) {
  if (_has_error(ps)) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ps->error, sizeof(ps->error), fmt, ap);
  va_end(ap);
}

// --- String allocation helpers ---

static char* _tok_strdup(ParseState* ps, Token* t) {
  int32_t len = t->end - t->start;
  char* s = malloc((size_t)len + 1);
  memcpy(s, ps->src + t->start, (size_t)len);
  s[len] = '\0';
  return s;
}

static char* _tok_strdup_skip(ParseState* ps, Token* t, int32_t skip) {
  int32_t len = t->end - t->start - skip;
  if (len < 0) {
    len = 0;
  }
  char* s = malloc((size_t)len + 1);
  memcpy(s, ps->src + t->start + skip, (size_t)len);
  s[len] = '\0';
  return s;
}

__attribute__((format(printf, 1, 2))) static char* _sfmt(const char* fmt, ...) {
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

static void _set_str(char** dst, char* s) {
  free(*dst);
  *dst = s;
}

// --- Token to ReToken conversion ---

static ReToken* _to_re_tokens(Token* tokens, int32_t count) {
  ReToken* out = malloc((size_t)count * sizeof(ReToken));
  for (int32_t i = 0; i < count; i++) {
    out[i] = (ReToken){tokens[i].id, tokens[i].start, tokens[i].end};
  }
  return out;
}

// --- Build string span from string token chunk ---

static StrSpan _build_str_span(TokenChunk* chunk) {
  if (tc_size(*chunk) == 0) {
    return (StrSpan){0, 0};
  }
  int32_t start = (*chunk)[0].start;
  int32_t end = (*chunk)[tc_size(*chunk) - 1].end;
  return (StrSpan){start, end - start};
}

// --- Pushdown automaton lexer ---

#define TOK_RE_AST_BASE 20000
#define TOK_STR_SPAN_BASE 30000

typedef enum {
  SCOPE_MAIN,
  SCOPE_VPA,
  SCOPE_PEG,
  SCOPE_RE,
  SCOPE_CHARCLASS,
  SCOPE_DQUOTE_STR,
  SCOPE_SQUOTE_STR,
} ScopeId;

typedef struct {
  ScopeId id;
  LexFunc lex_fn;
  int32_t end_token;
} ScopeConfig;

typedef struct {
  ParseState* ps;
  char* ustr;
  int32_t byte_size;
  UstrIter it;
  int32_t pos;
  ReAstNode* cc_asts; // darray - charclass ASTs for current re scope
} LexCtx;

static int32_t _next_cp(LexCtx* ctx) {
  if (ctx->it.byte_off >= ctx->byte_size) {
    return -2;
  }
  return ustr_iter_next(&ctx->it);
}

static TokenChunk _lex_scope(LexCtx* ctx, ScopeConfig* cfg) {
  TokenChunk chunk = NULL;
  tc_init(&chunk);
  
  int64_t state = 0;
  int32_t last_action = 0;
  int32_t tok_start = ctx->it.byte_off;
  int32_t tok_line = ctx->it.line;
  int32_t tok_col = ctx->it.col;
  
  while (ctx->pos < ctx->ps->src_len) {
    int32_t cp_byte = ctx->it.byte_off;
    int32_t cp_line = ctx->it.line;
    int32_t cp_col = ctx->it.col;
    int32_t cp = _next_cp(ctx);
    
    LexResult r = cfg->lex_fn(state, cp);
    
    if (r.action == LEX_ACTION_NOMATCH) {
      if (last_action > 0) {
        if (last_action == cfg->end_token) {
          ctx->pos = ctx->it.byte_off;
          return chunk;
        }
        if (last_action != TOK_COMMENT && last_action != TOK_SPACE) {
          tc_add(&chunk, (Token){last_action, tok_start, cp_byte, tok_line, tok_col});
        }
      }
      tok_start = cp_byte;
      tok_line = cp_line;
      tok_col = cp_col;
      last_action = 0;
      state = 0;
      if (cp == -2) break;
      r = cfg->lex_fn(0, cp);
      if (r.action == LEX_ACTION_NOMATCH) {
        tok_start = ctx->it.byte_off;
        tok_line = ctx->it.line;
        tok_col = ctx->it.col;
        ctx->pos = ctx->it.byte_off;
        continue;
      }
    }
    last_action = (int32_t)r.action;
    state = r.state;
    ctx->pos = ctx->it.byte_off;
    if (cp == -2) break;
  }
  return chunk;
}

static ReAstNode _lex_charclass(LexCtx* ctx, bool negated) {
  ScopeConfig cfg = {.id = SCOPE_CHARCLASS, .lex_fn = lex_charclass, .end_token = TOK_CLASS_END};
  TokenChunk chunk = _lex_scope(ctx, &cfg);
  ReToken* rtoks = _to_re_tokens(chunk, tc_size(chunk));
  ReAstNode ast = re_ast_build_charclass(ctx->ps->src, rtoks, tc_size(chunk), negated);
  free(rtoks);
  tc_free(&chunk);
  return ast;
}

static TokenChunk _lex_str(LexCtx* ctx, char quote) {
  LexFunc lex_fn = (quote == '"') ? lex_dquote_str : lex_squote_str;
  ScopeConfig cfg = {.id = (quote == '"') ? SCOPE_DQUOTE_STR : SCOPE_SQUOTE_STR, .lex_fn = lex_fn, .end_token = TOK_STR_END};
  return _lex_scope(ctx, &cfg);
}

static TokenChunk _lex_re(LexCtx* ctx) {
  ctx->cc_asts = darray_new(sizeof(ReAstNode), 0);
  TokenChunk chunk = NULL;
  tc_init(&chunk);
  
  int64_t state = 0;
  int32_t last_action = 0;
  int32_t tok_start = ctx->it.byte_off;
  int32_t tok_line = ctx->it.line;
  int32_t tok_col = ctx->it.col;
  
  while (ctx->pos < ctx->ps->src_len) {
    int32_t cp_byte = ctx->it.byte_off;
    int32_t cp_line = ctx->it.line;
    int32_t cp_col = ctx->it.col;
    int32_t cp = _next_cp(ctx);
    
    LexResult r = lex_re(state, cp);
    
    if (r.action == LEX_ACTION_NOMATCH) {
      if (last_action > 0) {
        if (last_action == TOK_RE_END) {
          ctx->pos = ctx->it.byte_off;
          return chunk;
        }
        if (last_action == TOK_NEG_CLASS_BEGIN || last_action == TOK_CLASS_BEGIN) {
          ReAstNode cc_ast = _lex_charclass(ctx, last_action == TOK_NEG_CLASS_BEGIN);
          darray_push(ctx->cc_asts, cc_ast);
          tc_add(&chunk, (Token){RE_AST_TOK_CHARCLASS_BASE + (int32_t)darray_size(ctx->cc_asts) - 1,
                                 tok_start, ctx->it.byte_off, tok_line, tok_col});
          tok_start = ctx->it.byte_off;
          tok_line = ctx->it.line;
          tok_col = ctx->it.col;
          last_action = 0;
          state = 0;
          continue;
        }
        if (last_action != TOK_COMMENT && last_action != TOK_SPACE) {
          tc_add(&chunk, (Token){last_action, tok_start, cp_byte, tok_line, tok_col});
        }
      }
      tok_start = cp_byte;
      tok_line = cp_line;
      tok_col = cp_col;
      last_action = 0;
      state = 0;
      if (cp == -2) break;
      r = lex_re(0, cp);
      if (r.action == LEX_ACTION_NOMATCH) {
        tok_start = ctx->it.byte_off;
        tok_line = ctx->it.line;
        tok_col = ctx->it.col;
        ctx->pos = ctx->it.byte_off;
        continue;
      }
    }
    last_action = (int32_t)r.action;
    state = r.state;
    ctx->pos = ctx->it.byte_off;
    if (cp == -2) break;
  }
  return chunk;
}

static bool _lex_with_dfa(ParseState* ps) {
  char* ustr = ustr_new((size_t)ps->src_len, ps->src);
  if (!ustr) {
    _error(ps, "invalid UTF-8 source");
    return false;
  }

  LexCtx ctx = {
    .ps = ps,
    .ustr = ustr,
    .byte_size = ustr_bytesize(ustr),
    .pos = 0,
    .cc_asts = NULL,
  };
  ustr_iter_init(&ctx.it, ustr, 0);

  LexFunc cur_lex = lex_main;
  int64_t state = 0;
  int32_t last_action = 0;
  int32_t tok_start = 0;
  int32_t tok_line = 1;
  int32_t tok_col = 1;

  while (ctx.pos < ps->src_len) {
    int32_t cp_byte = ctx.it.byte_off;
    int32_t cp_line = ctx.it.line;
    int32_t cp_col = ctx.it.col;
    int32_t cp = _next_cp(&ctx);

    LexResult r = cur_lex(state, cp);

    if (r.action == LEX_ACTION_NOMATCH) {
      if (last_action > 0) {
        if (last_action == TOK_SECTION_VPA || last_action == TOK_SECTION_PEG) {
          cur_lex = (last_action == TOK_SECTION_VPA) ? lex_vpa : lex_peg;
          tok_start = cp_byte;
          tok_line = cp_line;
          tok_col = cp_col;
          last_action = 0;
          state = 0;
          if (cp != -2) {
            r = cur_lex(0, cp);
            if (r.action != LEX_ACTION_NOMATCH) {
              last_action = (int32_t)r.action;
              state = r.state;
              ctx.pos = ctx.it.byte_off;
              continue;
            }
            tok_start = ctx.it.byte_off;
            tok_line = ctx.it.line;
            tok_col = ctx.it.col;
            ctx.pos = ctx.it.byte_off;
            continue;
          }
          break;
        }
        if (last_action == TOK_RE_BEGIN) {
          TokenChunk re_chunk = _lex_re(&ctx);
          int32_t re_ntok = tc_size(re_chunk);
          ReToken* re_rtoks = _to_re_tokens(re_chunk, re_ntok);
          ReAstNode* ast = re_ast_build_re(ps->src, re_rtoks, re_ntok, ctx.cc_asts,
                                           ctx.cc_asts ? (int32_t)darray_size(ctx.cc_asts) : 0);
          free(re_rtoks);
          darray_del(re_chunk);
          darray_del(ctx.cc_asts);
          ctx.cc_asts = NULL;
          if (!ps->re_asts) {
            ps->re_asts = darray_new(sizeof(ReAstNode*), 0);
          }
          darray_push(ps->re_asts, ast);
          tc_add(&ps->main_chunk, (Token){TOK_RE_AST_BASE + (int32_t)darray_size(ps->re_asts) - 1,
                                          tok_start, ctx.it.byte_off, tok_line, tok_col});
          tok_start = ctx.it.byte_off;
          tok_line = ctx.it.line;
          tok_col = ctx.it.col;
          last_action = 0;
          state = 0;
          continue;
        }
        if (last_action == TOK_STR_BEGIN) {
          char quote = ps->src[tok_start];
          TokenChunk str_chunk = _lex_str(&ctx, quote);
          StrSpan span = _build_str_span(&str_chunk);
          tc_free(&str_chunk);
          if (!ps->str_spans) {
            ps->str_spans = darray_new(sizeof(StrSpan), 0);
          }
          darray_push(ps->str_spans, span);
          tc_add(&ps->main_chunk, (Token){TOK_STR_SPAN_BASE + (int32_t)darray_size(ps->str_spans) - 1,
                                          tok_start, ctx.it.byte_off, tok_line, tok_col});
          tok_start = ctx.it.byte_off;
          tok_line = ctx.it.line;
          tok_col = ctx.it.col;
          last_action = 0;
          state = 0;
          continue;
        }
        if (last_action != TOK_COMMENT && last_action != TOK_SPACE) {
          tc_add(&ps->main_chunk, (Token){last_action, tok_start, cp_byte, tok_line, tok_col});
        }
      }
      tok_start = cp_byte;
      tok_line = cp_line;
      tok_col = cp_col;
      last_action = 0;
      state = 0;
      if (cp == -2) break;
      r = cur_lex(0, cp);
      if (r.action == LEX_ACTION_NOMATCH) {
        tok_start = ctx.it.byte_off;
        tok_line = ctx.it.line;
        tok_col = ctx.it.col;
        ctx.pos = ctx.it.byte_off;
        continue;
      }
    }
    last_action = (int32_t)r.action;
    state = r.state;
    ctx.pos = ctx.it.byte_off;
    if (cp == -2) break;
  }

  if (last_action > 0 && last_action != TOK_COMMENT && last_action != TOK_SPACE) {
    tc_add(&ps->main_chunk, (Token){last_action, tok_start, ctx.pos, tok_line, tok_col});
  }

  ustr_del(ustr);
  return true;
}
static Token* _peek(ParseState* ps) {
  if (ps->tpos < tc_size(ps->main_chunk)) {
    return &ps->main_chunk[ps->tpos];
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
  while (ps->tpos < tc_size(ps->main_chunk) && ps->main_chunk[ps->tpos].id == TOK_NL) {
    ps->tpos++;
  }
}

static bool _at_end(ParseState* ps) { return ps->tpos >= tc_size(ps->main_chunk); }

static bool _at(ParseState* ps, int32_t id) {
  Token* t = _peek(ps);
  return t && t->id == id;
}

// --- VPA unit helpers ---

static void _add_vpa_unit(VpaRule* rule, VpaUnit unit) {
  if (!rule->units) {
    rule->units = darray_new(sizeof(VpaUnit), 0);
  }
  darray_push(rule->units, unit);
}

// --- Shared VPA followup parsing ---

static bool _parse_vpa_scope_body(ParseState* ps, VpaRule* rule);

static void _parse_unit_followups(ParseState* ps, VpaUnit* unit, VpaRule* rule) {
  for (;;) {
    Token* ft = _peek(ps);
    if (!ft || ft->id == TOK_NL) {
      break;
    }
    if (ft->id == TOK_TOK_ID) {
      _next(ps);
      _set_str(&unit->name, _tok_strdup_skip(ps, ft, 1));
    } else if (ft->id == TOK_HOOK_BEGIN || ft->id == TOK_HOOK_END || ft->id == TOK_HOOK_FAIL ||
               ft->id == TOK_HOOK_UNPARSE) {
      _next(ps);
      unit->hook = ft->id;
    } else if (ft->id == TOK_USER_HOOK_ID) {
      _next(ps);
      _set_str(&unit->user_hook, _tok_strdup(ps, ft));
    } else if (ft->id == TOK_SCOPE_BEGIN) {
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

static bool _parse_vpa_regexp(ParseState* ps, VpaRule* rule) {
  Token* t = _peek(ps);
  if (!t || t->id < TOK_RE_AST_BASE || t->id >= TOK_RE_AST_BASE + (int32_t)darray_size(ps->re_asts)) {
    return false;
  }
  _next(ps);
  VpaUnit unit = {.kind = VPA_REGEXP};

  int32_t ast_idx = t->id - TOK_RE_AST_BASE;
  unit.re_ast = ps->re_asts[ast_idx];
  ps->re_asts[ast_idx] = NULL;

  int32_t mi = 0;
  for (int32_t i = t->start; i < t->end && mi < 3; i++) {
    char c = ps->src[i];
    if (c == '/') {
      break;
    }
    unit.mode[mi++] = c;
  }
  unit.mode[mi] = '\0';

  _parse_unit_followups(ps, &unit, rule);
  _add_vpa_unit(rule, unit);
  return true;
}

static bool _parse_vpa_str_span(ParseState* ps, VpaRule* rule) {
  Token* t = _peek(ps);
  if (!t || t->id < TOK_STR_SPAN_BASE || t->id >= TOK_STR_SPAN_BASE + (int32_t)darray_size(ps->str_spans)) {
    return false;
  }
  _next(ps);
  VpaUnit unit = {.kind = VPA_REGEXP};
  StrSpan span = ps->str_spans[t->id - TOK_STR_SPAN_BASE];
  unit.re_ast = re_ast_build_literal(ps->src, span.off, span.len);

  _parse_unit_followups(ps, &unit, rule);
  _add_vpa_unit(rule, unit);
  return true;
}

static bool _parse_vpa_ref(ParseState* ps, VpaRule* rule) {
  Token* t = _peek(ps);
  if (!t || t->id != TOK_VPA_ID) {
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
  if (!t || t->id != TOK_STATE_ID) {
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
  if (!t || t->id != TOK_MACRO_ID) {
    return false;
  }
  _next(ps);
  char* base = _tok_strdup_skip(ps, t, 1);
  VpaUnit unit = {.kind = VPA_REF, .name = _sfmt("*%s", base)};
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
  if (_parse_vpa_regexp(ps, rule) || _parse_vpa_str_span(ps, rule) || _parse_vpa_state_ref(ps, rule) ||
      _parse_vpa_ref(ps, rule) ||
      _parse_vpa_macro_ref(ps, rule) || _parse_vpa_pipe(ps) || _parse_vpa_nl(ps)) {
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
  if (!close || close->id != TOK_SCOPE_END) {
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
  if (!_at(ps, TOK_KW_KEYWORD)) {
    return false;
  }
  Token* kw_tok = _next(ps);

  Token* group_tok = _next(ps);
  if (!group_tok || group_tok->id != TOK_VPA_ID) {
    _error_at(ps, kw_tok, "expected keyword group name after %%keyword");
    return false;
  }
  char* group = _tok_strdup(ps, group_tok);

  while (!_at_end(ps) && !_at(ps, TOK_NL)) {
    Token* t = _peek(ps);
    if (t->id < TOK_STR_SPAN_BASE) {
      break;
    }
    _next(ps);
    StrSpan span = ps->str_spans[t->id - TOK_STR_SPAN_BASE];
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
  if (!_at(ps, TOK_KW_IGNORE)) {
    return false;
  }
  _next(ps);
  while (!_at_end(ps) && !_at(ps, TOK_NL)) {
    Token* t = _peek(ps);
    if (t->id != TOK_TOK_ID) {
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
  if (!_at(ps, TOK_KW_STATE)) {
    return false;
  }
  Token* state_tok = _next(ps);
  Token* t = _next(ps);
  if (!t || t->id != TOK_STATE_ID) {
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
  if (!_at(ps, TOK_KW_EFFECT)) {
    return false;
  }
  Token* effect_tok = _next(ps);

  Token* hook_tok = _next(ps);
  if (!hook_tok || hook_tok->id != TOK_USER_HOOK_ID) {
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
    if (t->id == TOK_OPS_PIPE) {
      _next(ps);
      continue;
    }
    if (t->id == TOK_TOK_ID || t->id == TOK_HOOK_BEGIN || t->id == TOK_HOOK_END || t->id == TOK_HOOK_FAIL ||
        t->id == TOK_HOOK_UNPARSE) {
      _next(ps);
      darray_push(ed.effects, t->id);
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
    if (t->id == TOK_NL) {
      ps->tpos++;
      continue;
    }
    if (t->id == TOK_PEG_ID || t->id == TOK_PEG_TOK_ID || t->id == TOK_PEG_ASSIGN) {
      break;
    }
    if (_parse_keyword_decl(ps) || _parse_ignore_decl(ps) || _parse_state_decl(ps) || _parse_effect_decl(ps) ||
        _parse_macro_rule(ps) || _parse_vpa_rule(ps)) {
      if (_has_error(ps)) {
        return false;
      }
      continue;
    }
    if (_has_error(ps)) {
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

  if (t->id == TOK_PEG_ID) {
    _next(ps);
    unit->kind = PEG_ID;
    unit->name = _tok_strdup(ps, t);
  } else if (t->id == TOK_PEG_TOK_ID) {
    _next(ps);
    unit->kind = PEG_TOK;
    unit->name = _tok_strdup_skip(ps, t, 1);
  } else if (t->id >= TOK_STR_SPAN_BASE) {
    _next(ps);
    unit->kind = PEG_TOK;
    StrSpan span = ps->str_spans[t->id - TOK_STR_SPAN_BASE];
    int32_t len = span.len;
    char* s = malloc((size_t)len + 1);
    memcpy(s, ps->src + span.off, (size_t)len);
    s[len] = '\0';
    unit->name = s;
  } else if (t->id == TOK_BRANCHES_BEGIN) {
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
      if (tag_t && tag_t->id == TOK_TAG_ID) {
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
    if (!close || close->id != TOK_BRANCHES_END) {
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
    if (mt->id == TOK_PEG_QUESTION) {
      _next(ps);
      unit->multiplier = '?';
    } else if (mt->id == TOK_PEG_PLUS) {
      _next(ps);
      unit->multiplier = '+';
    } else if (mt->id == TOK_PEG_STAR) {
      _next(ps);
      unit->multiplier = '*';
    }

    if (unit->multiplier == '+' || unit->multiplier == '*') {
      if (_at(ps, TOK_PEG_LT)) {
        _next(ps);
        unit->interlace = calloc(1, sizeof(PegUnit));
        unit->interlace->kind = PEG_SEQ;
        if (!_parse_peg_seq(ps, unit->interlace)) {
          return false;
        }
        unit->ninterlace = 1;
        Token* gt = _peek(ps);
        if (!gt || gt->id != TOK_PEG_GT) {
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
    if (!t || !_is_peg_unit_start(t->id)) {
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
  if (!name_tok || name_tok->id != TOK_PEG_ID) {
    _error_at(ps, name_tok, "expected peg rule name");
    return false;
  }
  if (!_at(ps, TOK_PEG_ASSIGN)) {
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

// --- Post-processing: keyword expansion ---

static void _expand_kw_in_peg_unit(PegUnit* unit, ParseState* ps) {
  if (unit->kind == PEG_TOK && unit->name) {
    for (int32_t k = 0; k < (int32_t)darray_size(ps->keywords); k++) {
      KeywordEntry* kw = &ps->keywords[k];
      int32_t len = kw->lit_len;
      char lit[len + 1];
      memcpy(lit, ps->src + kw->lit_off, (size_t)len);
      lit[len] = '\0';
      if (strcmp(unit->name, lit) == 0) {
        _set_str(&unit->name, _sfmt("%s.%s", kw->group, lit));
        break;
      }
    }
  }
  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    _expand_kw_in_peg_unit(&unit->children[i], ps);
  }
  if (unit->interlace) {
    _expand_kw_in_peg_unit(unit->interlace, ps);
  }
}

static void _expand_keywords(ParseState* ps) {
  for (int32_t k = 0; k < (int32_t)darray_size(ps->keywords); k++) {
    KeywordEntry* kw = &ps->keywords[k];
    int32_t len = kw->lit_len;
    char lit[len + 1];
    memcpy(lit, ps->src + kw->lit_off, (size_t)len);
    lit[len] = '\0';

    char* tok_name = _sfmt("%s.%s", kw->group, lit);
    ReAstNode* ast = re_ast_build_literal(ps->src, kw->lit_off, kw->lit_len);

    bool added = false;
    for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules) && !added; i++) {
      VpaRule* rule = &ps->vpa_rules[i];
      if (rule->is_macro || !rule->is_scope) {
        continue;
      }
      for (int32_t j = 0; j < (int32_t)darray_size(rule->units); j++) {
        if (rule->units[j].kind == VPA_REF && rule->units[j].name && strcmp(rule->units[j].name, kw->group) == 0) {
          VpaUnit unit = {.kind = VPA_REGEXP, .re_ast = ast, .name = strdup(tok_name)};
          _add_vpa_unit(rule, unit);
          added = true;
          break;
        }
      }
    }

    if (!added) {
      for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules); i++) {
        if (strcmp(ps->vpa_rules[i].name, kw->group) == 0) {
          VpaUnit unit = {.kind = VPA_REGEXP, .re_ast = ast, .name = strdup(tok_name)};
          _add_vpa_unit(&ps->vpa_rules[i], unit);
          added = true;
          break;
        }
      }
    }

    if (!added) {
      re_ast_free(ast);
      free(ast);
    }
    free(tok_name);
  }

  for (int32_t p = 0; p < (int32_t)darray_size(ps->peg_rules); p++) {
    _expand_kw_in_peg_unit(&ps->peg_rules[p].seq, ps);
  }
}

// --- Post-processing: inline macros ---

static VpaRule* _find_macro(ParseState* ps, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules); i++) {
    if (ps->vpa_rules[i].is_macro && strcmp(ps->vpa_rules[i].name, name) == 0) {
      return &ps->vpa_rules[i];
    }
  }
  return NULL;
}

static VpaUnit _clone_vpa_unit(VpaUnit* src) {
  VpaUnit dst = *src;
  dst.name = src->name ? strdup(src->name) : NULL;
  dst.state_name = src->state_name ? strdup(src->state_name) : NULL;
  dst.user_hook = src->user_hook ? strdup(src->user_hook) : NULL;
  dst.re_ast = re_ast_clone(src->re_ast);
  if ((int32_t)darray_size(src->children) > 0) {
    dst.children = darray_new(sizeof(VpaUnit), darray_size(src->children));
    for (int32_t i = 0; i < (int32_t)darray_size(src->children); i++) {
      dst.children[i] = _clone_vpa_unit(&src->children[i]);
    }
  }
  return dst;
}

static void _inline_macros(ParseState* ps) {
  for (int32_t r = 0; r < (int32_t)darray_size(ps->vpa_rules); r++) {
    VpaRule* rule = &ps->vpa_rules[r];
    if (rule->is_macro) {
      continue;
    }
    for (int32_t i = 0; i < (int32_t)darray_size(rule->units); i++) {
      VpaUnit* unit = &rule->units[i];
      if (unit->kind != VPA_REF || !unit->name || unit->name[0] != '*') {
        continue;
      }
      VpaRule* macro = _find_macro(ps, unit->name + 1);
      if (!macro) {
        continue;
      }

      int32_t new_count = (int32_t)darray_size(rule->units) - 1 + (int32_t)darray_size(macro->units);
      int32_t tail = (int32_t)darray_size(rule->units) - i - 1;

      // Free the macro ref unit being replaced
      free(unit->name);
      free(unit->user_hook);

      rule->units = darray_grow(rule->units, (size_t)new_count);

      if (tail > 0 && (int32_t)darray_size(macro->units) != 1) {
        memmove(&rule->units[i + (int32_t)darray_size(macro->units)], &rule->units[i + 1],
                (size_t)tail * sizeof(VpaUnit));
      }

      for (int32_t m = 0; m < (int32_t)darray_size(macro->units); m++) {
        rule->units[i + m] = _clone_vpa_unit(&macro->units[m]);
      }
      i += (int32_t)darray_size(macro->units) - 1;
    }
  }
}

// --- Post-processing: PEG branch auto-tagging ---

static void _auto_tag_unit(ParseState* ps, PegRule* rule, PegUnit* unit) {
  if (unit->kind == PEG_BRANCHES) {
    for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
      PegUnit* branch = &unit->children[i];
      if (!branch->tag || branch->tag[0] == '\0') {
        if ((int32_t)darray_size(branch->children) > 0) {
          PegUnit* first = &branch->children[0];
          if ((first->kind == PEG_ID || first->kind == PEG_TOK) && first->name && first->name[0]) {
            free(branch->tag);
            branch->tag = strdup(first->name);
          }
        }
        if ((!branch->tag || branch->tag[0] == '\0') && darray_size(branch->children) == 0) {
          _error(ps, "epsilon branch in rule '%s' must have an explicit tag", rule->name);
        }
      }
    }

    for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
      for (int32_t j = i + 1; j < (int32_t)darray_size(unit->children); j++) {
        if (unit->children[i].tag && unit->children[i].tag[0] && unit->children[j].tag && unit->children[j].tag[0] &&
            strcmp(unit->children[i].tag, unit->children[j].tag) == 0) {
          _error(ps, "duplicate tag '%s' in rule '%s'", unit->children[i].tag, rule->name);
        }
      }
    }
  }

  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    _auto_tag_unit(ps, rule, &unit->children[i]);
  }
  if (unit->interlace) {
    _auto_tag_unit(ps, rule, unit->interlace);
  }
}

static void _auto_tag_branches(ParseState* ps) {
  for (int32_t r = 0; r < (int32_t)darray_size(ps->peg_rules); r++) {
    _auto_tag_unit(ps, &ps->peg_rules[r], &ps->peg_rules[r].seq);
  }
}

static void _check_cross_bracket_tags(ParseState* ps) {
  for (int32_t r = 0; r < (int32_t)darray_size(ps->peg_rules); r++) {
    PegRule* rule = &ps->peg_rules[r];
    char** tags = darray_new(sizeof(char*), 0);

    for (int32_t i = 0; i < (int32_t)darray_size(rule->seq.children); i++) {
      PegUnit* child = &rule->seq.children[i];
      if (child->kind != PEG_BRANCHES) {
        continue;
      }
      for (int32_t j = 0; j < (int32_t)darray_size(child->children); j++) {
        char* tag = child->children[j].tag;
        if (!tag || !tag[0]) {
          continue;
        }
        for (int32_t k = 0; k < (int32_t)darray_size(tags); k++) {
          if (strcmp(tags[k], tag) == 0) {
            _error(ps, "duplicate tag '%s' across bracket groups in rule '%s'", tag, rule->name);
            goto next_rule;
          }
        }
        darray_push(tags, tag);
      }
    }
  next_rule:
    darray_del(tags);
  }
}

// --- Post-processing: allocate PEG rule IDs per scope ---

static bool _is_vpa_scope_rule(VpaRule* rule) {
  if (rule->is_macro) {
    return false;
  }
  if (rule->is_scope) {
    return true;
  }
  for (int32_t i = 0; i < (int32_t)darray_size(rule->units); i++) {
    if (rule->units[i].kind == VPA_SCOPE) {
      return true;
    }
  }
  return false;
}

static VpaUnit* _get_scope_body(VpaRule* rule) {
  if (rule->is_scope) {
    return rule->units;
  }
  for (int32_t i = 0; i < (int32_t)darray_size(rule->units); i++) {
    if (rule->units[i].kind == VPA_SCOPE) {
      return rule->units[i].children;
    }
  }
  return NULL;
}

static void _assign_peg_scopes(ParseState* ps) {
  for (int32_t p = 0; p < (int32_t)darray_size(ps->peg_rules); p++) {
    PegRule* peg = &ps->peg_rules[p];
    for (int32_t v = 0; v < (int32_t)darray_size(ps->vpa_rules); v++) {
      if (_is_vpa_scope_rule(&ps->vpa_rules[v]) && strcmp(ps->vpa_rules[v].name, peg->name) == 0) {
        _set_str(&peg->scope, strdup(ps->vpa_rules[v].name));
        break;
      }
    }
  }
}

// --- Token set validation: VPA emit set vs PEG used set per scope ---

static bool _str_set_has(char** set, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(set); i++) {
    if (strcmp(set[i], name) == 0) {
      return true;
    }
  }
  return false;
}

static void _str_set_add(char*** set, const char* name) {
  if (!_str_set_has(*set, name)) {
    char* dup = strdup(name);
    darray_push(*set, dup);
  }
}

static void _str_set_free(char** set) {
  for (int32_t i = 0; i < (int32_t)darray_size(set); i++) {
    free(set[i]);
  }
  darray_del(set);
}

static VpaRule* _find_vpa_rule(ParseState* ps, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules); i++) {
    if (!ps->vpa_rules[i].is_macro && strcmp(ps->vpa_rules[i].name, name) == 0) {
      return &ps->vpa_rules[i];
    }
  }
  return NULL;
}

static void _collect_emit_set(ParseState* ps, VpaUnit* units, char*** set, char*** visited) {
  for (int32_t i = 0; i < (int32_t)darray_size(units); i++) {
    VpaUnit* u = &units[i];

    if (u->kind == VPA_REGEXP || u->kind == VPA_STATE) {
      if (u->name && u->name[0]) {
        _str_set_add(set, u->name);
      }
    } else if (u->kind == VPA_REF) {
      VpaRule* ref = u->name ? _find_vpa_rule(ps, u->name) : NULL;
      if (ref) {
        // Bare ref to another rule. If it's a scope, it's a scope transition — not a token.
        // If it's a plain rule, recurse into its units.
        if (!_is_vpa_scope_rule(ref)) {
          if (!_str_set_has(*visited, u->name)) {
            char* dup = strdup(u->name);
            darray_push(*visited, dup);
            _collect_emit_set(ps, ref->units, set, visited);
            // If the ref rule has match units without names, they emit the rule name as token.
            for (int32_t j = 0; j < (int32_t)darray_size(ref->units); j++) {
              VpaUnit* ru = &ref->units[j];
              if ((ru->kind == VPA_REGEXP || ru->kind == VPA_STATE) && (!ru->name || !ru->name[0])) {
                _str_set_add(set, ref->name);
              }
            }
          }
        }
      } else if (u->name && u->name[0]) {
        // Name doesn't match any rule — it was overwritten by @tok_id followup
        _str_set_add(set, u->name);
      }
    }
    // VPA_SCOPE children are sub-scopes, not tokens in this scope
  }
}

static PegRule* _find_peg_rule(ParseState* ps, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(ps->peg_rules); i++) {
    if (strcmp(ps->peg_rules[i].name, name) == 0) {
      return &ps->peg_rules[i];
    }
  }
  return NULL;
}

static void _collect_peg_used_set(PegUnit* unit, char*** set, ParseState* ps, char*** visited_rules) {
  if (unit->kind == PEG_TOK && unit->name) {
    _str_set_add(set, unit->name);
  }
  if (unit->kind == PEG_ID && unit->name) {
    if (!_str_set_has(*visited_rules, unit->name)) {
      VpaRule* vr = _find_vpa_rule(ps, unit->name);
      if (!vr || !_is_vpa_scope_rule(vr)) {
        char* dup = strdup(unit->name);
        darray_push(*visited_rules, dup);
        PegRule* ref = _find_peg_rule(ps, unit->name);
        if (ref) {
          _collect_peg_used_set(&ref->seq, set, ps, visited_rules);
        }
      }
    }
  }
  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    _collect_peg_used_set(&unit->children[i], set, ps, visited_rules);
  }
  if (unit->interlace) {
    _collect_peg_used_set(unit->interlace, set, ps, visited_rules);
  }
}

static bool _is_ignored(ParseState* ps, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(ps->ignores.names); i++) {
    if (strcmp(ps->ignores.names[i], name) == 0) {
      return true;
    }
  }
  return false;
}

static bool _validate_token_sets(ParseState* ps) {
  for (int32_t v = 0; v < (int32_t)darray_size(ps->vpa_rules); v++) {
    VpaRule* vpa_rule = &ps->vpa_rules[v];
    if (!_is_vpa_scope_rule(vpa_rule)) {
      continue;
    }
    VpaUnit* body = _get_scope_body(vpa_rule);
    if (!body) {
      continue;
    }

    // Find PEG rules for this scope
    bool has_peg = false;
    for (int32_t p = 0; p < (int32_t)darray_size(ps->peg_rules); p++) {
      if (strcmp(ps->peg_rules[p].name, vpa_rule->name) == 0) {
        has_peg = true;
        break;
      }
    }
    if (!has_peg) {
      continue;
    }

    // Collect VPA emit set for this scope (excluding ignored tokens)
    char** emit_set = darray_new(sizeof(char*), 0);
    char** visited = darray_new(sizeof(char*), 0);
    _collect_emit_set(ps, body, &emit_set, &visited);
    _str_set_free(visited);

    // Remove ignored tokens from emit set
    char** filtered_emit = darray_new(sizeof(char*), 0);
    for (int32_t i = 0; i < (int32_t)darray_size(emit_set); i++) {
      if (!_is_ignored(ps, emit_set[i])) {
        char* dup = strdup(emit_set[i]);
        darray_push(filtered_emit, dup);
      }
    }
    _str_set_free(emit_set);

    // Collect PEG used set: from the entry rule, transitively follow PEG_ID refs (but not into sub-scopes)
    char** used_set = darray_new(sizeof(char*), 0);
    PegRule* entry = _find_peg_rule(ps, vpa_rule->name);
    if (entry) {
      char** visited_rules = darray_new(sizeof(char*), 0);
      _collect_peg_used_set(&entry->seq, &used_set, ps, &visited_rules);
      _str_set_free(visited_rules);
    }

    // Check: every PEG token must exist in VPA emit set
    for (int32_t i = 0; i < (int32_t)darray_size(used_set); i++) {
      if (!_str_set_has(filtered_emit, used_set[i])) {
        _error(ps, "scope '%s': peg uses token @%s not emitted by vpa", vpa_rule->name, used_set[i]);
        _str_set_free(filtered_emit);
        _str_set_free(used_set);
        return false;
      }
    }

    // Check: every VPA token must be used in PEG
    for (int32_t i = 0; i < (int32_t)darray_size(filtered_emit); i++) {
      if (!_str_set_has(used_set, filtered_emit[i])) {
        _error(ps, "scope '%s': vpa emits token @%s not used by peg", vpa_rule->name, filtered_emit[i]);
        _str_set_free(filtered_emit);
        _str_set_free(used_set);
        return false;
      }
    }

    _str_set_free(filtered_emit);
    _str_set_free(used_set);
  }
  return true;
}

// --- Validations ---

static bool _validate(ParseState* ps) {
  bool has_vpa_main = false;
  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules); i++) {
    if (!ps->vpa_rules[i].is_macro && strcmp(ps->vpa_rules[i].name, "main") == 0) {
      has_vpa_main = true;
      break;
    }
  }
  if (!has_vpa_main) {
    _error(ps, "'main' rule must exist in [[vpa]]");
    return false;
  }

  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules); i++) {
    VpaRule* rule = &ps->vpa_rules[i];
    for (int32_t j = 0; j < (int32_t)darray_size(rule->units); j++) {
      VpaUnit* u = &rule->units[j];
      if (u->kind != VPA_STATE || !u->state_name || !u->state_name[0]) {
        continue;
      }
      bool found = false;
      for (int32_t s = 0; s < (int32_t)darray_size(ps->states); s++) {
        if (strcmp(ps->states[s].name, u->state_name) == 0) {
          found = true;
          break;
        }
      }
      if (!found) {
        _error(ps, "state '$%s' used in rule '%s' is not declared", u->state_name, rule->name);
        return false;
      }
    }
  }

  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules); i++) {
    VpaRule* rule = &ps->vpa_rules[i];
    if (!rule->is_scope || rule->is_macro || strcmp(rule->name, "main") == 0) {
      continue;
    }
    bool has_begin = false;
    bool has_end = false;
    for (int32_t j = 0; j < (int32_t)darray_size(rule->units); j++) {
      VpaUnit* u = &rule->units[j];
      if (u->hook == TOK_HOOK_BEGIN) {
        has_begin = true;
      }
      if (u->hook == TOK_HOOK_END) {
        has_end = true;
      }
      if (u->user_hook && u->user_hook[0]) {
        for (int32_t e = 0; e < (int32_t)darray_size(ps->effects); e++) {
          if (strcmp(ps->effects[e].hook_name, u->user_hook) == 0) {
            for (int32_t ef = 0; ef < (int32_t)darray_size(ps->effects[e].effects); ef++) {
              if (ps->effects[e].effects[ef] == TOK_HOOK_BEGIN) {
                has_begin = true;
              }
              if (ps->effects[e].effects[ef] == TOK_HOOK_END) {
                has_end = true;
              }
            }
          }
        }
      }
    }
    if (has_begin && !has_end) {
      _error(ps, "scope '%s' missing .end", rule->name);
      return false;
    }
  }

  if (!_validate_token_sets(ps)) {
    return false;
  }

  return true;
}

// --- Free helpers ---

static void _free_vpa_unit(VpaUnit* unit) {
  if (unit->re_ast) {
    re_ast_free(unit->re_ast);
    free(unit->re_ast);
  }
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
  tc_free(&ps->main_chunk);
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
  for (int32_t i = 0; i < (int32_t)darray_size(ps->re_asts); i++) {
    if (ps->re_asts[i]) {
      re_ast_free(ps->re_asts[i]);
      free(ps->re_asts[i]);
    }
  }
  darray_del(ps->re_asts);
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

// --- Public API ---

void parse_nest(const char* src, HeaderWriter* header_writer, IrWriter* ir_writer) {
  ParseState ps = {0};
  ps.src = src;
  ps.src_len = (int32_t)strlen(src);

  if (!_lex_with_dfa(&ps)) {
    goto fail;
  }

  ps.tpos = 0;
  if (!_parse_vpa_section(&ps)) {
    goto fail;
  }
  if (!_parse_peg_section(&ps)) {
    goto fail;
  }
  if (!_at_end(&ps)) {
    _error_at(&ps, _peek(&ps), "unexpected token after end of input");
    goto fail;
  }

  _inline_macros(&ps);
  _expand_keywords(&ps);
  _auto_tag_branches(&ps);
  _check_cross_bracket_tags(&ps);
  _assign_peg_scopes(&ps);
  if (!_validate(&ps)) {
    goto fail;
  }
  if (_has_error(&ps)) {
    goto fail;
  }

  peg_gen(
      &(PegGenInput){
          .rules = ps.peg_rules,
      },
      header_writer, ir_writer);

  vpa_gen(
      &(VpaGenInput){
          .rules = ps.vpa_rules,
          .keywords = ps.keywords,
          .states = ps.states,
          .effects = ps.effects,
          .peg_rules = ps.peg_rules,
          .src = ps.src,
      },
      header_writer, ir_writer);

  _free_state(&ps);
  return;

fail:
  fprintf(stderr, "parse error: %s\n", ps.error);
  _free_state(&ps);
}
