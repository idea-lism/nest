// Parser for .nest syntax files.
// Pushdown automaton lexer (calls generated DFA functions from parse_gen)
// + recursive descent for structure.
// Produces processed data, then invokes vpa_gen() and peg_gen().

#include "parse.h"
#include "darray.h"
#include "peg.h"
#include "re.h"
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
extern LexResult lex_peg(int64_t state, int64_t cp);
extern LexResult lex_re(int64_t state, int64_t cp);
extern LexResult lex_re_ref(int64_t state, int64_t cp);
extern LexResult lex_re_str(int64_t state, int64_t cp);
extern LexResult lex_charclass(int64_t state, int64_t cp);
extern LexResult lex_keyword_str(int64_t state, int64_t cp);

#define LEX_ACTION_NOMATCH (-2)

typedef LexResult (*LexFunc)(int64_t, int64_t);

// --- String span (content region within quotes) ---

typedef struct {
  int32_t off;
  int32_t len;
} StrSpan;

// --- Error reporting ---

bool parse_has_error(ParseState* ps) { return ps->error[0] != '\0'; }

static void _error_at(ParseState* ps, Token* t, const char* fmt, ...) {
  if (parse_has_error(ps)) {
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

__attribute__((format(printf, 1, 2))) char* parseparse_sfmt(const char* fmt, ...) {
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

void parseparse_set_str(char** dst, char* s) {
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

typedef struct {
  ParseState* ps;
  char* ustr;
  int32_t byte_size;
  int32_t pos;
  UstrIter it;
  ReAstNode* cc_asts; // darray - charclass ASTs for current re scope
  const char* last_quote;
} LexCtx;

typedef struct {
  ScopeId id;
  LexFunc lex_fn;
} ScopeConfig;

static int32_t _next_cp(LexCtx* ctx) {
  if (ctx->it.byte_off >= ctx->byte_size) {
    return -2;
  }
  return ustr_iter_next(&ctx->it);
}

static TokenChunk _lex_main(LexCtx* ctx);
static TokenChunk _lex_vpa(LexCtx* ctx);
static TokenChunk _lex_peg(LexCtx* ctx);
static TokenChunk _lex_re(LexCtx* ctx);
static TokenChunk _lex_re_ref(LexCtx* ctx);
static TokenChunk _lex_re_str(LexCtx* ctx);
static TokenChunk _lex_charclass(LexCtx* ctx);
static TokenChunk _lex_keyword_str(LexCtx* ctx);

static TokenChunk _lex_scope(LexCtx* ctx, ScopeId scope_id) {
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

    LexResult r = cfg.lex_fn(state, cp);

    if (r.action == LEX_ACTION_NOMATCH) {
      if (last_action == TOK_END) {
        ctx->pos = ctx->it.byte_off;
        return chunk;
      } else if (last_action == TOK_IGNORE) {
        break;
      } else if (last_action == TOK_UNPARSE_END) {
        // special handling for vpa .unparse .end
        // TODO unparse a token
        ctx->pos = ctx->it.byte_off;
        return chunk;
      } else if (last_action <= SCOPE_COUNT) { // scope id
        if (last_action == SCOPE_KEYWORD_STR || last_action == SCOPE_RE_STR) {
          ctx->last_quote = ctx->ps->src + tok_start;
        }
        _lex_scope(ctx, last_action);
      } else {
        tc_add(&chunk, (Token){last_action, tok_start, cp_byte, tok_line, tok_col});
      }

      if (cp == LEX_CP_EOF) {
        // TODO: pop all to make chunks complete
        return chunk;
      }

      tok_start = cp_byte;
      tok_line = cp_line;
      tok_col = cp_col;
      last_action = 0;
      state = 0;
    } else {
      state = r.state;
      last_action = r.action;
    }
  }
  return chunk;
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
      parse_set_str(&unit->name, _tok_strdup_skip(ps, ft, 1));
    } else if (ft->id == TOK_HOOK_BEGIN || ft->id == TOK_HOOK_END || ft->id == TOK_HOOK_FAIL ||
               ft->id == TOK_HOOK_UNPARSE) {
      _next(ps);
      unit->hook = ft->id;
    } else if (ft->id == TOK_USER_HOOK_ID) {
      _next(ps);
      parse_set_str(&unit->user_hook, _tok_strdup(ps, ft));
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
        parse_set_str(&unit->name, parse_sfmt("%s.%s", kw->group, lit));
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

    char* tok_name = parse_sfmt("%s.%s", kw->group, lit);
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

ParseState* parse_state_new(void) {
  ParseState* ps = calloc(1, sizeof(ParseState));
  return ps;
}

void parse_state_del(ParseState* ps) {
  if (!ps) return;
  _free_state(ps);
  free(ps);
}

const char* parse_get_error(ParseState* ps) {
  if (!ps) return NULL;
  return ps->error[0] ? ps->error : NULL;
}

// --- Public API ---

bool parse_nest(ParseState* ps, const char* src) {
  ps->src = src;
  ps->src_len = (int32_t)strlen(src);

  if (!_lex_scope(ps)) {
    return false;
  }

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

  _inline_macros(ps);
  _expand_keywords(ps);

  return true;
}
