#include "../src/darray.h"
#include "../src/parse.h"
#include "../src/post_process.h"
#include "../src/re_ir.h"
#include "../src/symtab.h"
#include "../src/ustr.h"
#include "../src/vpa.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

// Helper: init tokens, hooks, scope_names, and rule_names symtabs as parse_nest would
static void _init_symtabs(ParseState* ps) {
  symtab_init(&ps->tokens, 1);
  symtab_init(&ps->hooks, 1);
  symtab_intern(&ps->hooks, ".begin");   // HOOK_ID_BEGIN = 1
  symtab_intern(&ps->hooks, ".end");     // HOOK_ID_END = 2
  symtab_intern(&ps->hooks, ".fail");    // HOOK_ID_FAIL = 3
  symtab_intern(&ps->hooks, ".unparse"); // HOOK_ID_UNPARSE = 4
  symtab_intern(&ps->hooks, ".noop");    // HOOK_ID_NOOP = 5
  symtab_init(&ps->scope_names, 0);
  symtab_init(&ps->rule_names, 0);
}

// Helper: get the first token name from a VpaUnit's action_units, or NULL
static const char* _unit_tok_name(VpaUnit* u, Symtab* tokens) {
  for (size_t i = 0; i < darray_size(u->action_units); i++) {
    if (u->action_units[i] > 0) {
      return symtab_get(tokens, u->action_units[i]);
    }
  }
  return NULL;
}

// Helper: make a VPA_RE unit with an re and optional token (interned in symtab)
static VpaUnit _make_re_unit(int32_t ch, const char* tok_name, Symtab* tokens) {
  VpaUnit u = {.kind = VPA_RE, .re = re_ir_new(), .call_scope_id = -1};
  if (ch >= 0) {
    u.re = re_ir_emit_ch(u.re, ch);
  }
  if (tok_name && tokens) {
    int32_t tok_id = symtab_intern(tokens, tok_name);
    u.action_units = darray_new(sizeof(int32_t), 0);
    darray_push(u.action_units, tok_id);
  }
  return u;
}

// Helper: make a VPA_RE unit with action_units containing a hook
static VpaUnit _make_re_unit_hook(int32_t ch, int32_t hook_id) {
  VpaUnit u = {.kind = VPA_RE, .re = re_ir_new(), .call_scope_id = -1};
  if (ch >= 0) {
    u.re = re_ir_emit_ch(u.re, ch);
  }
  u.action_units = darray_new(sizeof(int32_t), 0);
  int32_t au = -hook_id;
  darray_push(u.action_units, au);
  return u;
}

// Helper: make a VPA_MACRO_REF unit (for *macro references)
static VpaUnit _make_macro_ref(const char* name) {
  VpaUnit u = {.kind = VPA_MACRO_REF};
  u.macro_name = strdup(name);
  return u;
}

// Helper: make a VPA_CALL unit (scope reference by id)
static VpaUnit _make_call_unit(int32_t scope_id) {
  VpaUnit u = {.kind = VPA_CALL, .call_scope_id = scope_id};
  return u;
}

// Helper: make a scope (no leader, children provided externally)
static VpaScope _make_scope(const char* name, int32_t scope_id, bool is_macro) {
  VpaScope s = {.scope_id = scope_id,
                .name = strdup(name),
                .leader = {0},
                .children = darray_new(sizeof(VpaUnit), 0),
                .is_macro = is_macro};
  return s;
}

// Helper: make a scoped rule with a leader
static VpaScope _make_scoped(const char* name, int32_t scope_id, VpaUnit leader) {
  VpaScope s = {
      .scope_id = scope_id, .name = strdup(name), .leader = leader, .children = darray_new(sizeof(VpaUnit), 0)};
  return s;
}

// ============================================================================
// pp_inline_macros
// ============================================================================

TEST(test_inline_macros) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);

  // main = { /a/ @word *ws }
  VpaScope main_s = _make_scope("main", 0, false);
  VpaUnit word_u = _make_re_unit('a', "word", &ps->tokens);
  darray_push(main_s.children, word_u);
  VpaUnit ws_ref = _make_macro_ref("ws");
  darray_push(main_s.children, ws_ref);
  darray_push(ps->vpa_scopes, main_s);

  // *ws = { /[ ]/ @space /\n/ @nl }
  VpaScope ws_macro = _make_scope("ws", 100, true);
  VpaUnit sp_u = _make_re_unit(' ', "space", &ps->tokens);
  darray_push(ws_macro.children, sp_u);
  VpaUnit nl_u = _make_re_unit('\n', "nl", &ps->tokens);
  darray_push(ws_macro.children, nl_u);
  darray_push(ps->vpa_scopes, ws_macro);

  bool ok = pp_inline_macros(ps);
  assert(ok);

  bool found_space = false;
  bool found_nl = false;
  VpaScope* mr = &ps->vpa_scopes[0];
  for (size_t j = 0; j < darray_size(mr->children); j++) {
    VpaUnit* u = &mr->children[j];
    const char* tok = _unit_tok_name(u, &ps->tokens);
    if (u->kind == VPA_RE && tok) {
      if (strcmp(tok, "space") == 0) {
        found_space = true;
      }
      if (strcmp(tok, "nl") == 0) {
        found_nl = true;
      }
    }
  }
  assert(found_space);
  assert(found_nl);

  parse_state_del(ps);
}

TEST(test_inline_macros_missing) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);

  VpaScope main_s = _make_scope("main", 0, false);
  VpaUnit ref = _make_macro_ref("nonexistent");
  darray_push(main_s.children, ref);
  darray_push(ps->vpa_scopes, main_s);

  bool ok = pp_inline_macros(ps);
  assert(!ok);
  assert(parse_has_error(ps));
  assert(strstr(parse_get_error(ps), "not found") != NULL);

  parse_state_del(ps);
}

// ============================================================================
// pp_auto_tag_branches
// ============================================================================

TEST(test_auto_tag_branches) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  // item = [ @id  |  @num ]   (no explicit tags)
  PegRule rule = {.global_id = symtab_intern(&ps->rule_names, "item"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  rule.body.children = darray_new(sizeof(PegUnit), 0);

  PegUnit branches = {.kind = PEG_BRANCHES};
  branches.children = darray_new(sizeof(PegUnit), 0);

  PegUnit b1 = {.kind = PEG_SEQ};
  b1.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t1 = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "id")};
  darray_push(b1.children, t1);
  darray_push(branches.children, b1);

  PegUnit b2 = {.kind = PEG_SEQ};
  b2.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t2 = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "num")};
  darray_push(b2.children, t2);
  darray_push(branches.children, b2);

  darray_push(rule.body.children, branches);
  darray_push(ps->peg_rules, rule);

  bool ok = pp_auto_tag_branches(ps);
  assert(ok);

  PegUnit* br = &ps->peg_rules[0].body.children[0];
  assert(br->kind == PEG_BRANCHES);
  assert(br->children[0].tag && strcmp(br->children[0].tag, "id") == 0);
  assert(br->children[1].tag && strcmp(br->children[1].tag, "num") == 0);

  parse_state_del(ps);
}

TEST(test_auto_tag_many_tags) {
  // >64 tags are supported via multi-bucket
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  PegRule rule = {.global_id = symtab_intern(&ps->rule_names, "big"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  rule.body.children = darray_new(sizeof(PegUnit), 0);

  PegUnit branches = {.kind = PEG_BRANCHES};
  branches.children = darray_new(sizeof(PegUnit), 0);
  for (int32_t i = 0; i < 65; i++) {
    char name[16];
    snprintf(name, sizeof(name), "t%d", i);
    PegUnit b = {.kind = PEG_SEQ};
    b.children = darray_new(sizeof(PegUnit), 0);
    PegUnit t = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, name)};
    darray_push(b.children, t);
    darray_push(branches.children, b);
  }
  darray_push(rule.body.children, branches);
  darray_push(ps->peg_rules, rule);

  bool ok = pp_auto_tag_branches(ps);
  assert(ok);
  assert(!parse_has_error(ps));

  parse_state_del(ps);
}

TEST(test_auto_tag_rule_name_too_long) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  char long_name[256];
  memset(long_name, 'a', 129);
  long_name[129] = '\0';

  PegRule rule = {.global_id = symtab_intern(&ps->rule_names, long_name), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  rule.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "x")};
  darray_push(rule.body.children, t);
  darray_push(ps->peg_rules, rule);

  bool ok = pp_auto_tag_branches(ps);
  assert(!ok);
  assert(parse_has_error(ps));
  assert(strstr(parse_get_error(ps), "128") != NULL);

  parse_state_del(ps);
}

// ============================================================================
// pp_check_duplicate_tags
// ============================================================================

TEST(test_duplicate_tag_error) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  // item = [ @tok_a : dup  |  @tok_b : dup ]
  PegRule rule = {.global_id = symtab_intern(&ps->rule_names, "item"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  rule.body.children = darray_new(sizeof(PegUnit), 0);

  PegUnit branches = {.kind = PEG_BRANCHES};
  branches.children = darray_new(sizeof(PegUnit), 0);

  PegUnit b1 = {.kind = PEG_SEQ, .tag = strdup("dup")};
  b1.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t1 = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "tok_a")};
  darray_push(b1.children, t1);
  darray_push(branches.children, b1);

  PegUnit b2 = {.kind = PEG_SEQ, .tag = strdup("dup")};
  b2.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t2 = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "tok_b")};
  darray_push(b2.children, t2);
  darray_push(branches.children, b2);

  darray_push(rule.body.children, branches);
  darray_push(ps->peg_rules, rule);

  bool ok = pp_check_duplicate_tags(ps);
  assert(!ok);
  assert(parse_has_error(ps));
  assert(strstr(parse_get_error(ps), "dup") != NULL);

  parse_state_del(ps);
}

TEST(test_no_duplicate_tags) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  PegRule rule = {.global_id = symtab_intern(&ps->rule_names, "item"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  rule.body.children = darray_new(sizeof(PegUnit), 0);

  PegUnit branches = {.kind = PEG_BRANCHES};
  branches.children = darray_new(sizeof(PegUnit), 0);

  PegUnit b1 = {.kind = PEG_SEQ, .tag = strdup("alpha")};
  b1.children = darray_new(sizeof(PegUnit), 0);
  darray_push(branches.children, b1);

  PegUnit b2 = {.kind = PEG_SEQ, .tag = strdup("beta")};
  b2.children = darray_new(sizeof(PegUnit), 0);
  darray_push(branches.children, b2);

  darray_push(rule.body.children, branches);
  darray_push(ps->peg_rules, rule);

  bool ok = pp_check_duplicate_tags(ps);
  assert(ok);

  parse_state_del(ps);
}

// ============================================================================
// pp_detect_left_recursions
// ============================================================================

static void _make_left_rec_peg(ParseState* ps) {
  _init_symtabs(ps);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  // main = expr
  PegRule main_rule = {.global_id = symtab_intern(&ps->rule_names, "main"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  main_rule.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit expr_ref = {.kind = PEG_CALL, .id = symtab_intern(&ps->rule_names, "expr")};
  darray_push(main_rule.body.children, expr_ref);
  darray_push(ps->peg_rules, main_rule);

  // expr = [ @id : id | expr @id : binop ]
  PegRule expr_rule = {.global_id = symtab_intern(&ps->rule_names, "expr"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  expr_rule.body.children = darray_new(sizeof(PegUnit), 0);

  PegUnit branches = {.kind = PEG_BRANCHES};
  branches.children = darray_new(sizeof(PegUnit), 0);

  PegUnit branch1 = {.kind = PEG_SEQ, .tag = strdup("id")};
  branch1.children = darray_new(sizeof(PegUnit), 0);
  PegUnit tok_id1 = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "id")};
  darray_push(branch1.children, tok_id1);
  darray_push(branches.children, branch1);

  PegUnit branch2 = {.kind = PEG_SEQ, .tag = strdup("binop")};
  branch2.children = darray_new(sizeof(PegUnit), 0);
  PegUnit self_ref = {.kind = PEG_CALL, .id = symtab_intern(&ps->rule_names, "expr")};
  darray_push(branch2.children, self_ref);
  PegUnit tok_id2 = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "id")};
  darray_push(branch2.children, tok_id2);
  darray_push(branches.children, branch2);

  darray_push(expr_rule.body.children, branches);
  darray_push(ps->peg_rules, expr_rule);
}

TEST(test_detect_left_recursion) {
  ParseState* ps = parse_state_new();
  _make_left_rec_peg(ps);

  bool ok = pp_detect_left_recursions(ps);
  assert(!ok);
  assert(parse_has_error(ps));
  assert(strstr(parse_get_error(ps), "left recursion") != NULL);

  parse_state_del(ps);
}

TEST(test_no_left_recursion) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  // main = @id+ @num?
  PegRule main_rule = {.global_id = symtab_intern(&ps->rule_names, "main"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  main_rule.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit tok1 = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "id"), .multiplier = '+'};
  darray_push(main_rule.body.children, tok1);
  PegUnit tok2 = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "num"), .multiplier = '?'};
  darray_push(main_rule.body.children, tok2);
  darray_push(ps->peg_rules, main_rule);

  bool ok = pp_detect_left_recursions(ps);
  assert(ok);

  parse_state_del(ps);
}

// ============================================================================
// pp_detect_left_recursions: interlace nullable
// ============================================================================

TEST(test_interlace_both_nullable) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  // opt = @tok?
  PegRule opt_rule = {.global_id = symtab_intern(&ps->rule_names, "opt"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  opt_rule.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit opt_tok = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "tok"), .multiplier = '?'};
  darray_push(opt_rule.body.children, opt_tok);
  darray_push(ps->peg_rules, opt_rule);

  // main = opt*<opt>  (both lhs and rhs call nullable rule)
  PegRule main_rule = {.global_id = symtab_intern(&ps->rule_names, "main"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  main_rule.body.children = darray_new(sizeof(PegUnit), 0);
  int32_t opt_id = symtab_find(&ps->rule_names, "opt");
  PegUnit interlace = {
      .kind = PEG_CALL, .id = opt_id, .multiplier = '*', .interlace_rhs_kind = PEG_CALL, .interlace_rhs_id = opt_id};
  darray_push(main_rule.body.children, interlace);
  darray_push(ps->peg_rules, main_rule);

  bool ok = pp_detect_left_recursions(ps);
  assert(!ok);
  assert(parse_has_error(ps));
  assert(strstr(parse_get_error(ps), "interlace") != NULL);
  assert(strstr(parse_get_error(ps), "nullable") != NULL);

  parse_state_del(ps);
}

TEST(test_interlace_rhs_not_nullable) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  // main = @id*<@comma>  (rhs is a token, never nullable)
  PegRule main_rule = {.global_id = symtab_intern(&ps->rule_names, "main"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  main_rule.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit interlace = {.kind = PEG_TERM,
                       .id = symtab_intern(&ps->tokens, "id"),
                       .multiplier = '*',
                       .interlace_rhs_kind = PEG_TERM,
                       .interlace_rhs_id = symtab_intern(&ps->tokens, "comma")};
  darray_push(main_rule.body.children, interlace);
  darray_push(ps->peg_rules, main_rule);

  bool ok = pp_detect_left_recursions(ps);
  assert(ok);

  parse_state_del(ps);
}

TEST(test_interlace_indirect_nullable) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  // inner = @tok?
  PegRule inner = {.global_id = symtab_intern(&ps->rule_names, "inner"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  inner.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit inner_tok = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "tok"), .multiplier = '?'};
  darray_push(inner.body.children, inner_tok);
  darray_push(ps->peg_rules, inner);

  // wrap = inner  (nullable through call chain)
  PegRule wrap = {.global_id = symtab_intern(&ps->rule_names, "wrap"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  wrap.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit wrap_call = {.kind = PEG_CALL, .id = symtab_find(&ps->rule_names, "inner")};
  darray_push(wrap.body.children, wrap_call);
  darray_push(ps->peg_rules, wrap);

  // main = wrap+<wrap>  (both lhs and rhs nullable through call chain)
  PegRule main_rule = {.global_id = symtab_intern(&ps->rule_names, "main"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  main_rule.body.children = darray_new(sizeof(PegUnit), 0);
  int32_t wrap_id = symtab_find(&ps->rule_names, "wrap");
  PegUnit interlace = {
      .kind = PEG_CALL, .id = wrap_id, .multiplier = '+', .interlace_rhs_kind = PEG_CALL, .interlace_rhs_id = wrap_id};
  darray_push(main_rule.body.children, interlace);
  darray_push(ps->peg_rules, main_rule);

  bool ok = pp_detect_left_recursions(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "interlace") != NULL);

  parse_state_del(ps);
}

TEST(test_undefined_call) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  // main = missing_rule
  PegRule main_rule = {.global_id = symtab_intern(&ps->rule_names, "main"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  main_rule.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit call = {.kind = PEG_CALL, .id = symtab_intern(&ps->rule_names, "missing_rule")};
  darray_push(main_rule.body.children, call);
  darray_push(ps->peg_rules, main_rule);

  bool ok = pp_detect_left_recursions(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "not defined") != NULL);
  assert(strstr(parse_get_error(ps), "missing_rule") != NULL);

  parse_state_del(ps);
}

// ============================================================================
// pp_inline_macros: literal naming
// ============================================================================

TEST(test_inline_macros_literals) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);

  // main = { /a/ @word *kw }
  VpaScope main_s = _make_scope("main", 0, false);
  VpaUnit word_u = _make_re_unit('a', "word", &ps->tokens);
  darray_push(main_s.children, word_u);
  VpaUnit kw_ref = _make_macro_ref("kw");
  darray_push(main_s.children, kw_ref);
  darray_push(ps->vpa_scopes, main_s);

  // *kw = @{ "+" "-" } — literals are pre-named by _parse_lit_scope during parsing
  // Simulate: create units with lit.+ and lit.- tokens already resolved in the unified tokens symtab
  int32_t lit_plus = symtab_intern(&ps->tokens, "lit.+");
  int32_t lit_minus = symtab_intern(&ps->tokens, "lit.-");

  VpaScope kw_macro = _make_scope("kw", 100, true);
  VpaUnit plus_u = {.kind = VPA_RE, .re = re_ir_new(), .action_units = darray_new(sizeof(int32_t), 0)};
  plus_u.re = re_ir_emit_ch(plus_u.re, '+');
  darray_push(plus_u.action_units, lit_plus);
  darray_push(kw_macro.children, plus_u);
  VpaUnit minus_u = {.kind = VPA_RE, .re = re_ir_new(), .action_units = darray_new(sizeof(int32_t), 0)};
  minus_u.re = re_ir_emit_ch(minus_u.re, '-');
  darray_push(minus_u.action_units, lit_minus);
  darray_push(kw_macro.children, minus_u);
  darray_push(ps->vpa_scopes, kw_macro);

  bool ok = pp_inline_macros(ps);
  assert(ok);

  // literal tokens should still be in the unified tokens symtab
  assert(symtab_find(&ps->tokens, "lit.+") >= 0);
  assert(symtab_find(&ps->tokens, "lit.-") >= 0);

  // inlined units in main should have the literal tokens
  VpaScope* mr = &ps->vpa_scopes[0];
  bool found_plus = false;
  bool found_minus = false;
  for (size_t j = 0; j < darray_size(mr->children); j++) {
    VpaUnit* u = &mr->children[j];
    const char* tok = _unit_tok_name(u, &ps->tokens);
    if (u->kind == VPA_RE && tok) {
      if (strcmp(tok, "lit.+") == 0) {
        found_plus = true;
      }
      if (strcmp(tok, "lit.-") == 0) {
        found_minus = true;
      }
    }
  }
  assert(found_plus);
  assert(found_minus);

  parse_state_del(ps);
}

// Recursive macro dependency: *a references *b, *b references *a → error
TEST(test_inline_macros_recursive_error) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);

  // main = { *a }
  VpaScope main_s = _make_scope("main", 0, false);
  VpaUnit a_ref = _make_macro_ref("a");
  darray_push(main_s.children, a_ref);
  darray_push(ps->vpa_scopes, main_s);

  // *a = { /x/ @tok_x *b }
  VpaScope a_macro = _make_scope("a", 100, true);
  VpaUnit ax = _make_re_unit('x', "tok_x", &ps->tokens);
  darray_push(a_macro.children, ax);
  VpaUnit b_ref = _make_macro_ref("b");
  darray_push(a_macro.children, b_ref);
  darray_push(ps->vpa_scopes, a_macro);

  // *b = { /y/ @tok_y *a }  (cycle: b → a → b)
  VpaScope b_macro = _make_scope("b", 101, true);
  VpaUnit by = _make_re_unit('y', "tok_y", &ps->tokens);
  darray_push(b_macro.children, by);
  VpaUnit a_ref2 = _make_macro_ref("a");
  darray_push(b_macro.children, a_ref2);
  darray_push(ps->vpa_scopes, b_macro);

  bool ok = pp_inline_macros(ps);
  assert(!ok);
  assert(parse_has_error(ps));
  assert(strstr(parse_get_error(ps), "recursive") != NULL);

  parse_state_del(ps);
}

// Cascaded expanding: *inner = { /y/ @tok_y }, *outer = { /x/ @tok_x *inner }
// Definitions in reverse order (outer first, inner second) to exercise topo-sort.
// main = { /z/ @tok_z *outer }
// After inlining: main should have tok_z, tok_x, tok_y
TEST(test_inline_macros_cascaded) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);

  // main = { /z/ @tok_z *outer }
  VpaScope main_s = _make_scope("main", 0, false);
  VpaUnit mz = _make_re_unit('z', "tok_z", &ps->tokens);
  darray_push(main_s.children, mz);
  VpaUnit outer_ref = _make_macro_ref("outer");
  darray_push(main_s.children, outer_ref);
  darray_push(ps->vpa_scopes, main_s);

  // *outer = { /x/ @tok_x *inner }  (defined BEFORE inner to test ordering)
  VpaScope outer_macro = _make_scope("outer", 100, true);
  VpaUnit ox = _make_re_unit('x', "tok_x", &ps->tokens);
  darray_push(outer_macro.children, ox);
  VpaUnit inner_ref = _make_macro_ref("inner");
  darray_push(outer_macro.children, inner_ref);
  darray_push(ps->vpa_scopes, outer_macro);

  // *inner = { /y/ @tok_y }
  VpaScope inner_macro = _make_scope("inner", 101, true);
  VpaUnit iy = _make_re_unit('y', "tok_y", &ps->tokens);
  darray_push(inner_macro.children, iy);
  darray_push(ps->vpa_scopes, inner_macro);

  bool ok = pp_inline_macros(ps);
  assert(ok);

  // main should now have: tok_z, tok_x, tok_y (all fully expanded)
  VpaScope* mr = &ps->vpa_scopes[0];
  int32_t found = 0;
  bool found_z = false, found_x = false, found_y = false;
  for (size_t j = 0; j < darray_size(mr->children); j++) {
    VpaUnit* u = &mr->children[j];
    assert(u->kind != VPA_MACRO_REF); // no unresolved macro refs
    const char* tok = _unit_tok_name(u, &ps->tokens);
    if (u->kind == VPA_RE && tok) {
      if (strcmp(tok, "tok_z") == 0) {
        found_z = true;
        found++;
      }
      if (strcmp(tok, "tok_x") == 0) {
        found_x = true;
        found++;
      }
      if (strcmp(tok, "tok_y") == 0) {
        found_y = true;
        found++;
      }
    }
  }
  assert(found_z);
  assert(found_x);
  assert(found_y);
  assert(found == 3);

  parse_state_del(ps);
}

// ============================================================================
// pp_validate_vpa_scopes
// ============================================================================

TEST(test_validate_vpa_missing_main) {
  ParseState* ps = parse_state_new();
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);
  ps->effect_decls = darray_new(sizeof(EffectDecl), 0);

  VpaScope scope = _make_scope("other", 0, false);
  darray_push(ps->vpa_scopes, scope);

  bool ok = pp_validate_vpa_scopes(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "main") != NULL);
  assert(strstr(parse_get_error(ps), "vpa") != NULL);

  parse_state_del(ps);
}

TEST(test_validate_vpa_duplicate_scope) {
  ParseState* ps = parse_state_new();
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);
  ps->effect_decls = darray_new(sizeof(EffectDecl), 0);

  VpaScope main_s = _make_scope("main", 0, false);
  darray_push(ps->vpa_scopes, main_s);

  // foo: scoped with leader='(' .begin, children: ')' .end
  VpaUnit leader1 = _make_re_unit_hook('(', HOOK_ID_BEGIN);
  VpaScope dup1 = _make_scoped("foo", 1, leader1);
  VpaUnit end1 = _make_re_unit_hook(')', HOOK_ID_END);
  darray_push(dup1.children, end1);
  darray_push(ps->vpa_scopes, dup1);

  VpaUnit leader2 = _make_re_unit_hook('[', HOOK_ID_BEGIN);
  VpaScope dup2 = _make_scoped("foo", 1, leader2);
  VpaUnit end2 = _make_re_unit_hook(']', HOOK_ID_END);
  darray_push(dup2.children, end2);
  darray_push(ps->vpa_scopes, dup2);

  bool ok = pp_validate_vpa_scopes(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "duplicate") != NULL);
  assert(strstr(parse_get_error(ps), "foo") != NULL);

  parse_state_del(ps);
}

TEST(test_validate_vpa_leading_re_empty) {
  ParseState* ps = parse_state_new();
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);
  ps->effect_decls = darray_new(sizeof(EffectDecl), 0);

  VpaScope main_s = _make_scope("main", 0, false);
  darray_push(ps->vpa_scopes, main_s);

  // scope with empty leading re (no char emitted)
  VpaUnit leader = _make_re_unit_hook(-1, HOOK_ID_BEGIN); // -1 ch = empty re
  VpaScope scope = _make_scoped("str", 1, leader);
  VpaUnit body_end = _make_re_unit_hook('"', HOOK_ID_END);
  darray_push(scope.children, body_end);
  darray_push(ps->vpa_scopes, scope);

  bool ok = pp_validate_vpa_scopes(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "leading") != NULL);
  assert(strstr(parse_get_error(ps), "at least 1") != NULL);

  parse_state_del(ps);
}

TEST(test_validate_vpa_empty_re_needs_hook) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);
  ps->effect_decls = darray_new(sizeof(EffectDecl), 0);

  // main with an empty fallback RE that has no .end or .fail
  VpaScope main_s = _make_scope("main", 0, false);
  VpaUnit fallback = _make_re_unit(-1, "bad", &ps->tokens); // empty re, named "bad", no hook
  darray_push(main_s.children, fallback);
  darray_push(ps->vpa_scopes, main_s);

  bool ok = pp_validate_vpa_scopes(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "empty") != NULL);
  assert(strstr(parse_get_error(ps), ".end") != NULL);

  parse_state_del(ps);
}

TEST(test_validate_vpa_empty_re_with_end_ok) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);
  ps->effect_decls = darray_new(sizeof(EffectDecl), 0);

  VpaScope main_s = _make_scope("main", 0, false);
  VpaUnit tok = _make_re_unit('a', "id", &ps->tokens);
  darray_push(main_s.children, tok);
  darray_push(ps->vpa_scopes, main_s);

  bool ok = pp_validate_vpa_scopes(ps);
  assert(ok);

  parse_state_del(ps);
}

TEST(test_validate_vpa_two_empty_re) {
  ParseState* ps = parse_state_new();
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);
  ps->effect_decls = darray_new(sizeof(EffectDecl), 0);

  VpaScope main_s = _make_scope("main", 0, false);
  VpaUnit e1 = _make_re_unit_hook(-1, HOOK_ID_END);
  darray_push(main_s.children, e1);
  VpaUnit e2 = _make_re_unit_hook(-1, HOOK_ID_FAIL);
  darray_push(main_s.children, e2);
  darray_push(ps->vpa_scopes, main_s);

  bool ok = pp_validate_vpa_scopes(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "at most 1") != NULL);

  parse_state_del(ps);
}

// ============================================================================
// pp_validate_peg_rules
// ============================================================================

TEST(test_validate_peg_missing_main) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  PegRule pr = {.global_id = symtab_intern(&ps->rule_names, "helper"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  pr.body.children = darray_new(sizeof(PegUnit), 0);
  darray_push(ps->peg_rules, pr);

  bool ok = pp_validate_peg_rules(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "main") != NULL);
  assert(strstr(parse_get_error(ps), "peg") != NULL);

  parse_state_del(ps);
}

TEST(test_validate_peg_duplicate_rule) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  int32_t main_id = symtab_intern(&ps->rule_names, "main");
  int32_t expr_id = symtab_intern(&ps->rule_names, "expr");

  PegRule main_r = {.global_id = main_id, .scope_id = -1, .body = {.kind = PEG_SEQ}};
  main_r.body.children = darray_new(sizeof(PegUnit), 0);
  darray_push(ps->peg_rules, main_r);

  PegRule dup1 = {.global_id = expr_id, .scope_id = -1, .body = {.kind = PEG_SEQ}};
  dup1.body.children = darray_new(sizeof(PegUnit), 0);
  darray_push(ps->peg_rules, dup1);

  PegRule dup2 = {.global_id = expr_id, .scope_id = -1, .body = {.kind = PEG_SEQ}};
  dup2.body.children = darray_new(sizeof(PegUnit), 0);
  darray_push(ps->peg_rules, dup2);

  bool ok = pp_validate_peg_rules(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "duplicate") != NULL);
  assert(strstr(parse_get_error(ps), "expr") != NULL);

  parse_state_del(ps);
}

// ============================================================================
// pp_match_scopes (token set validation)
// ============================================================================

TEST(test_match_scopes_token_mismatch) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);
  ps->effect_decls = darray_new(sizeof(EffectDecl), 0);
  symtab_init(&ps->ignores.names, 0);

  // VPA main emits @id only
  VpaScope vr = _make_scope("main", 0, false);
  VpaUnit u = _make_re_unit('a', "id", &ps->tokens);
  darray_push(vr.children, u);
  darray_push(ps->vpa_scopes, vr);

  // PEG main uses @id and @num (num not emitted by VPA)
  PegRule pr = {.global_id = symtab_intern(&ps->rule_names, "main"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  pr.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t1 = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "id")};
  darray_push(pr.body.children, t1);
  PegUnit t2 = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "num")};
  darray_push(pr.body.children, t2);
  darray_push(ps->peg_rules, pr);

  bool ok = pp_match_scopes(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "num") != NULL);

  parse_state_del(ps);
}

TEST(test_match_scopes_ok) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);
  ps->effect_decls = darray_new(sizeof(EffectDecl), 0);
  symtab_init(&ps->ignores.names, 0);

  // VPA main emits @id, @num, @space; %ignore @space
  VpaScope vr = _make_scope("main", 0, false);
  VpaUnit u1 = _make_re_unit('a', "id", &ps->tokens);
  darray_push(vr.children, u1);
  VpaUnit u2 = _make_re_unit('0', "num", &ps->tokens);
  darray_push(vr.children, u2);
  VpaUnit u3 = _make_re_unit(' ', "space", &ps->tokens);
  darray_push(vr.children, u3);
  darray_push(ps->vpa_scopes, vr);

  symtab_intern(&ps->ignores.names, "space");

  // PEG main uses @id and @num
  PegRule pr = {.global_id = symtab_intern(&ps->rule_names, "main"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  pr.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t1 = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "id")};
  darray_push(pr.body.children, t1);
  PegUnit t2 = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "num")};
  darray_push(pr.body.children, t2);
  darray_push(ps->peg_rules, pr);

  bool ok = pp_match_scopes(ps);
  assert(ok);

  // Check that VpaScopes were built
  assert(ps->vpa_scopes != NULL);
  assert((int32_t)darray_size(ps->vpa_scopes) == 1);
  assert(strcmp(ps->vpa_scopes[0].name, "main") == 0);

  parse_state_del(ps);
}

TEST(test_match_scopes_scope_ref_in_sets) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  symtab_intern(&ps->scope_names, "main"); // 0
  symtab_intern(&ps->scope_names, "foo");  // 1
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);
  ps->effect_decls = darray_new(sizeof(EffectDecl), 0);
  symtab_init(&ps->ignores.names, 0);

  // VPA: foo = /.../ { @a ... }  (a scope with leader)
  VpaUnit foo_leader = _make_re_unit_hook('(', HOOK_ID_BEGIN);
  VpaScope foo_vr = _make_scoped("foo", 1, foo_leader);
  VpaUnit foo_a = _make_re_unit('x', "a", &ps->tokens);
  darray_push(foo_vr.children, foo_a);
  VpaUnit foo_end = _make_re_unit_hook(')', HOOK_ID_END);
  darray_push(foo_vr.children, foo_end);
  darray_push(ps->vpa_scopes, foo_vr);

  // VPA: main = { @b foo }  (emit_set = {@b, foo})
  VpaScope main_vr = _make_scope("main", 0, false);
  VpaUnit main_b = _make_re_unit('b', "b", &ps->tokens);
  darray_push(main_vr.children, main_b);
  VpaUnit main_foo_ref = _make_call_unit(1);
  darray_push(main_vr.children, main_foo_ref);
  darray_push(ps->vpa_scopes, main_vr);

  // PEG: foo = @a  (foo is a scope, won't be expanded)
  PegRule foo_pr = {.global_id = symtab_intern(&ps->rule_names, "foo"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  foo_pr.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit foo_tok = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "a")};
  darray_push(foo_pr.body.children, foo_tok);
  darray_push(ps->peg_rules, foo_pr);

  // PEG: main = foo @b  (used_set = {foo, @b})
  PegRule main_pr = {.global_id = symtab_intern(&ps->rule_names, "main"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  main_pr.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit main_foo = {.kind = PEG_CALL, .id = symtab_intern(&ps->rule_names, "foo")};
  darray_push(main_pr.body.children, main_foo);
  PegUnit main_tok = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "b")};
  darray_push(main_pr.body.children, main_tok);
  darray_push(ps->peg_rules, main_pr);

  bool ok = pp_match_scopes(ps);
  assert(ok);

  parse_state_del(ps);
}

TEST(test_match_scopes_scope_ref_mismatch) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  symtab_intern(&ps->scope_names, "main"); // 0
  symtab_intern(&ps->scope_names, "foo");  // 1
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);
  ps->effect_decls = darray_new(sizeof(EffectDecl), 0);
  symtab_init(&ps->ignores.names, 0);

  // VPA: foo = /.../ { @a ... }  (a scope with leader)
  VpaUnit foo_leader = _make_re_unit_hook('(', HOOK_ID_BEGIN);
  VpaScope foo_vr = _make_scoped("foo", 1, foo_leader);
  VpaUnit foo_a = _make_re_unit('x', "a", &ps->tokens);
  darray_push(foo_vr.children, foo_a);
  VpaUnit foo_end = _make_re_unit_hook(')', HOOK_ID_END);
  darray_push(foo_vr.children, foo_end);
  darray_push(ps->vpa_scopes, foo_vr);

  // VPA: main = { @b foo }  (emit_set = {@b, foo})
  VpaScope main_vr = _make_scope("main", 0, false);
  VpaUnit main_b = _make_re_unit('b', "b", &ps->tokens);
  darray_push(main_vr.children, main_b);
  VpaUnit main_foo_ref = _make_call_unit(1);
  darray_push(main_vr.children, main_foo_ref);
  darray_push(ps->vpa_scopes, main_vr);

  // PEG: main = @b  (used_set = {@b}, missing foo)
  PegRule main_pr = {.global_id = symtab_intern(&ps->rule_names, "main"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  main_pr.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit main_tok = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "b")};
  darray_push(main_pr.body.children, main_tok);
  darray_push(ps->peg_rules, main_pr);

  bool ok = pp_match_scopes(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "foo") != NULL);

  parse_state_del(ps);
}

// VPA_CALL to a non-parser scope should expand its emit_set rather than
// keeping the opaque scope_id.  main calls inner (no peg parser), inner
// emits @c.  PEG main uses @b @c -- sets should match after expansion.
TEST(test_match_scopes_expand_non_parser_scope) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  symtab_intern(&ps->scope_names, "main");  // 0
  symtab_intern(&ps->scope_names, "inner"); // 1
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);
  ps->effect_decls = darray_new(sizeof(EffectDecl), 0);
  symtab_init(&ps->ignores.names, 0);

  // VPA: inner = { @c }  (no peg parser for inner)
  VpaScope inner_vr = _make_scope("inner", 1, false);
  VpaUnit inner_c = _make_re_unit('c', "c", &ps->tokens);
  darray_push(inner_vr.children, inner_c);
  darray_push(ps->vpa_scopes, inner_vr);

  // VPA: main = { @b inner }  (emit_set should expand to {@b, @c})
  VpaScope main_vr = _make_scope("main", 0, false);
  VpaUnit main_b = _make_re_unit('b', "b", &ps->tokens);
  darray_push(main_vr.children, main_b);
  VpaUnit main_inner_ref = _make_call_unit(1);
  darray_push(main_vr.children, main_inner_ref);
  darray_push(ps->vpa_scopes, main_vr);

  // PEG: inner_helper = @c  (a plain peg rule, not a scope)
  PegRule inner_pr = {
      .global_id = symtab_intern(&ps->rule_names, "inner_helper"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  inner_pr.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit inner_tok = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "c")};
  darray_push(inner_pr.body.children, inner_tok);
  darray_push(ps->peg_rules, inner_pr);

  // PEG: main = @b inner_helper  (used_set expands inner_helper -> {@b, @c})
  PegRule main_pr = {.global_id = symtab_intern(&ps->rule_names, "main"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  main_pr.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit main_tok_b = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "b")};
  darray_push(main_pr.body.children, main_tok_b);
  PegUnit main_call_ih = {.kind = PEG_CALL, .id = symtab_intern(&ps->rule_names, "inner_helper")};
  darray_push(main_pr.body.children, main_call_ih);
  darray_push(ps->peg_rules, main_pr);

  bool ok = pp_match_scopes(ps);
  assert(ok);

  parse_state_del(ps);
}

// Emit set must not expand a scope that has a mapping PEG parser.
// VPA: main = { @b parser_scope no_parser_scope }
// parser_scope has a PEG rule  → emit_set keeps it as scope_id
// no_parser_scope has no PEG rule, emits @c → emit_set expands to @c
// PEG: main = @b parser_scope @c
TEST(test_match_scopes_no_expand_parser_scope) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  symtab_intern(&ps->scope_names, "main");            // 0
  symtab_intern(&ps->scope_names, "parser_scope");    // 1
  symtab_intern(&ps->scope_names, "no_parser_scope"); // 2
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);
  ps->effect_decls = darray_new(sizeof(EffectDecl), 0);
  symtab_init(&ps->ignores.names, 0);

  // VPA: parser_scope = /x/ .begin @tok_x { /y/ @tok_y .end }
  VpaUnit ps_leader = _make_re_unit_hook('x', HOOK_ID_BEGIN);
  VpaScope ps_vr = _make_scoped("parser_scope", 1, ps_leader);
  VpaUnit ps_y = _make_re_unit('y', "tok_y", &ps->tokens);
  darray_push(ps_vr.children, ps_y);
  VpaUnit ps_end = _make_re_unit_hook(')', HOOK_ID_END);
  darray_push(ps_vr.children, ps_end);
  darray_push(ps->vpa_scopes, ps_vr);

  // VPA: no_parser_scope = { @c }  (no PEG parser → should be expanded)
  VpaScope np_vr = _make_scope("no_parser_scope", 2, false);
  VpaUnit np_c = _make_re_unit('c', "c", &ps->tokens);
  darray_push(np_vr.children, np_c);
  darray_push(ps->vpa_scopes, np_vr);

  // VPA: main = { @b parser_scope no_parser_scope }
  VpaScope main_vr = _make_scope("main", 0, false);
  VpaUnit main_b = _make_re_unit('b', "b", &ps->tokens);
  darray_push(main_vr.children, main_b);
  VpaUnit main_ps_ref = _make_call_unit(1);
  darray_push(main_vr.children, main_ps_ref);
  VpaUnit main_np_ref = _make_call_unit(2);
  darray_push(main_vr.children, main_np_ref);
  darray_push(ps->vpa_scopes, main_vr);

  // PEG: parser_scope = @tok_y  (this makes parser_scope a PEG scope)
  PegRule ps_pr = {
      .global_id = symtab_intern(&ps->rule_names, "parser_scope"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  ps_pr.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit ps_tok = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "tok_y")};
  darray_push(ps_pr.body.children, ps_tok);
  darray_push(ps->peg_rules, ps_pr);

  // PEG: main = @b parser_scope @c
  // emit_set = {@b, parser_scope, @c} (parser_scope NOT expanded, no_parser_scope expanded)
  PegRule main_pr = {.global_id = symtab_intern(&ps->rule_names, "main"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  main_pr.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit main_tok_b = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "b")};
  darray_push(main_pr.body.children, main_tok_b);
  PegUnit main_call_ps = {.kind = PEG_CALL, .id = symtab_intern(&ps->rule_names, "parser_scope")};
  darray_push(main_pr.body.children, main_call_ps);
  PegUnit main_tok_c = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "c")};
  darray_push(main_pr.body.children, main_tok_c);
  darray_push(ps->peg_rules, main_pr);

  bool ok = pp_match_scopes(ps);
  assert(ok);

  parse_state_del(ps);
}

// Recursive non-parser scope must not cause infinite recursion in emit_set expansion.
// VPA: main = { @b rec }  where rec = { @c rec }  (rec has no PEG parser)
// emit_set for main should expand rec → {@c} without looping.
// PEG: main = @b @c
TEST(test_match_scopes_expand_recursive_no_loop) {
  ParseState* ps = parse_state_new();
  _init_symtabs(ps);
  symtab_intern(&ps->scope_names, "main"); // 0
  symtab_intern(&ps->scope_names, "rec");  // 1
  ps->vpa_scopes = darray_new(sizeof(VpaScope), 0);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);
  ps->effect_decls = darray_new(sizeof(EffectDecl), 0);
  symtab_init(&ps->ignores.names, 0);

  // VPA: rec = { @c rec }  (no peg parser, self-referential)
  VpaScope rec_vr = _make_scope("rec", 1, false);
  VpaUnit rec_c = _make_re_unit('c', "c", &ps->tokens);
  darray_push(rec_vr.children, rec_c);
  VpaUnit rec_self = _make_call_unit(1); // rec calls itself
  darray_push(rec_vr.children, rec_self);
  darray_push(ps->vpa_scopes, rec_vr);

  // VPA: main = { @b rec }
  VpaScope main_vr = _make_scope("main", 0, false);
  VpaUnit main_b = _make_re_unit('b', "b", &ps->tokens);
  darray_push(main_vr.children, main_b);
  VpaUnit main_rec_ref = _make_call_unit(1);
  darray_push(main_vr.children, main_rec_ref);
  darray_push(ps->vpa_scopes, main_vr);

  // PEG: main = @b @c  (emit_set after expanding rec = {@b, @c})
  PegRule main_pr = {.global_id = symtab_intern(&ps->rule_names, "main"), .scope_id = -1, .body = {.kind = PEG_SEQ}};
  main_pr.body.children = darray_new(sizeof(PegUnit), 0);
  PegUnit main_tok_b = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "b")};
  darray_push(main_pr.body.children, main_tok_b);
  PegUnit main_tok_c = {.kind = PEG_TERM, .id = symtab_intern(&ps->tokens, "c")};
  darray_push(main_pr.body.children, main_tok_c);
  darray_push(ps->peg_rules, main_pr);

  // This must complete without stack overflow
  bool ok = pp_match_scopes(ps);
  assert(ok);

  parse_state_del(ps);
}

int main(void) {
  printf("test_post_process:\n");

  RUN(test_inline_macros);
  RUN(test_inline_macros_missing);
  RUN(test_inline_macros_literals);
  RUN(test_inline_macros_recursive_error);
  RUN(test_inline_macros_cascaded);
  RUN(test_auto_tag_branches);
  RUN(test_auto_tag_many_tags);
  RUN(test_auto_tag_rule_name_too_long);
  RUN(test_duplicate_tag_error);
  RUN(test_no_duplicate_tags);
  RUN(test_detect_left_recursion);
  RUN(test_no_left_recursion);
  RUN(test_interlace_both_nullable);
  RUN(test_interlace_rhs_not_nullable);
  RUN(test_interlace_indirect_nullable);
  RUN(test_undefined_call);
  RUN(test_validate_vpa_missing_main);
  RUN(test_validate_vpa_duplicate_scope);
  RUN(test_validate_vpa_leading_re_empty);
  RUN(test_validate_vpa_empty_re_needs_hook);
  RUN(test_validate_vpa_empty_re_with_end_ok);
  RUN(test_validate_vpa_two_empty_re);
  RUN(test_validate_peg_missing_main);
  RUN(test_validate_peg_duplicate_rule);
  RUN(test_match_scopes_token_mismatch);
  RUN(test_match_scopes_ok);
  RUN(test_match_scopes_scope_ref_in_sets);
  RUN(test_match_scopes_scope_ref_mismatch);
  RUN(test_match_scopes_expand_non_parser_scope);
  RUN(test_match_scopes_no_expand_parser_scope);
  RUN(test_match_scopes_expand_recursive_no_loop);

  printf("all ok\n");
  return 0;
}
