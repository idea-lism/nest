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

// ============================================================================
// pp_inline_macros
// ============================================================================

TEST(test_inline_macros) {
  ParseState* ps = parse_state_new();
  ps->vpa_rules = darray_new(sizeof(VpaRule), 0);

  // main = { /a/ @word *ws }
  VpaRule main_rule = {.name = strdup("main"), .is_scope = true};
  main_rule.units = darray_new(sizeof(VpaUnit), 0);
  VpaUnit word_u = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("word")};
  word_u.re = re_ir_emit_ch(word_u.re, 'a');
  darray_push(main_rule.units, word_u);
  VpaUnit ws_ref = {.kind = VPA_REF, .name = strdup("*ws")};
  darray_push(main_rule.units, ws_ref);
  darray_push(ps->vpa_rules, main_rule);

  // *ws = { /[ ]/ @space /\n/ @nl }
  VpaRule ws_macro = {.name = strdup("ws"), .is_scope = true, .is_macro = true};
  ws_macro.units = darray_new(sizeof(VpaUnit), 0);
  VpaUnit sp_u = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("space")};
  sp_u.re = re_ir_emit_ch(sp_u.re, ' ');
  darray_push(ws_macro.units, sp_u);
  VpaUnit nl_u = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("nl")};
  nl_u.re = re_ir_emit_ch(nl_u.re, '\n');
  darray_push(ws_macro.units, nl_u);
  darray_push(ps->vpa_rules, ws_macro);

  bool ok = pp_inline_macros(ps);
  assert(ok);

  bool found_space = false;
  bool found_nl = false;
  VpaRule* mr = &ps->vpa_rules[0];
  for (int32_t j = 0; j < (int32_t)darray_size(mr->units); j++) {
    VpaUnit* u = &mr->units[j];
    if (u->kind == VPA_REGEXP && u->name) {
      if (strcmp(u->name, "space") == 0) {
        found_space = true;
      }
      if (strcmp(u->name, "nl") == 0) {
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
  ps->vpa_rules = darray_new(sizeof(VpaRule), 0);

  VpaRule rule = {.name = strdup("main"), .is_scope = true};
  rule.units = darray_new(sizeof(VpaUnit), 0);
  VpaUnit ref = {.kind = VPA_REF, .name = strdup("*nonexistent")};
  darray_push(rule.units, ref);
  darray_push(ps->vpa_rules, rule);

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
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  // item = [ @id  |  @num ]   (no explicit tags)
  PegRule rule = {.name = strdup("item"), .seq = {.kind = PEG_SEQ}};
  rule.seq.children = darray_new(sizeof(PegUnit), 0);

  PegUnit branches = {.kind = PEG_BRANCHES};
  branches.children = darray_new(sizeof(PegUnit), 0);

  PegUnit b1 = {.kind = PEG_SEQ};
  b1.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t1 = {.kind = PEG_TOK, .name = strdup("id")};
  darray_push(b1.children, t1);
  darray_push(branches.children, b1);

  PegUnit b2 = {.kind = PEG_SEQ};
  b2.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t2 = {.kind = PEG_TOK, .name = strdup("num")};
  darray_push(b2.children, t2);
  darray_push(branches.children, b2);

  darray_push(rule.seq.children, branches);
  darray_push(ps->peg_rules, rule);

  bool ok = pp_auto_tag_branches(ps);
  assert(ok);

  PegUnit* br = &ps->peg_rules[0].seq.children[0];
  assert(br->kind == PEG_BRANCHES);
  assert(br->children[0].tag && strcmp(br->children[0].tag, "id") == 0);
  assert(br->children[1].tag && strcmp(br->children[1].tag, "num") == 0);

  parse_state_del(ps);
}

// ============================================================================
// pp_check_duplicate_tags
// ============================================================================

TEST(test_duplicate_tag_error) {
  ParseState* ps = parse_state_new();
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  // item = [ @tok_a : dup  |  @tok_b : dup ]
  PegRule rule = {.name = strdup("item"), .seq = {.kind = PEG_SEQ}};
  rule.seq.children = darray_new(sizeof(PegUnit), 0);

  PegUnit branches = {.kind = PEG_BRANCHES};
  branches.children = darray_new(sizeof(PegUnit), 0);

  PegUnit b1 = {.kind = PEG_SEQ, .tag = strdup("dup")};
  b1.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t1 = {.kind = PEG_TOK, .name = strdup("tok_a")};
  darray_push(b1.children, t1);
  darray_push(branches.children, b1);

  PegUnit b2 = {.kind = PEG_SEQ, .tag = strdup("dup")};
  b2.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t2 = {.kind = PEG_TOK, .name = strdup("tok_b")};
  darray_push(b2.children, t2);
  darray_push(branches.children, b2);

  darray_push(rule.seq.children, branches);
  darray_push(ps->peg_rules, rule);

  bool ok = pp_check_duplicate_tags(ps);
  assert(!ok);
  assert(parse_has_error(ps));
  assert(strstr(parse_get_error(ps), "dup") != NULL);

  parse_state_del(ps);
}

TEST(test_no_duplicate_tags) {
  ParseState* ps = parse_state_new();
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  PegRule rule = {.name = strdup("item"), .seq = {.kind = PEG_SEQ}};
  rule.seq.children = darray_new(sizeof(PegUnit), 0);

  PegUnit branches = {.kind = PEG_BRANCHES};
  branches.children = darray_new(sizeof(PegUnit), 0);

  PegUnit b1 = {.kind = PEG_SEQ, .tag = strdup("alpha")};
  b1.children = darray_new(sizeof(PegUnit), 0);
  darray_push(branches.children, b1);

  PegUnit b2 = {.kind = PEG_SEQ, .tag = strdup("beta")};
  b2.children = darray_new(sizeof(PegUnit), 0);
  darray_push(branches.children, b2);

  darray_push(rule.seq.children, branches);
  darray_push(ps->peg_rules, rule);

  bool ok = pp_check_duplicate_tags(ps);
  assert(ok);

  parse_state_del(ps);
}

// ============================================================================
// pp_detect_left_recursions
// ============================================================================

static void _make_left_rec_peg(ParseState* ps) {
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  // main = expr
  PegRule main_rule = {.name = strdup("main"), .seq = {.kind = PEG_SEQ}};
  main_rule.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit expr_ref = {.kind = PEG_ID, .name = strdup("expr")};
  darray_push(main_rule.seq.children, expr_ref);
  darray_push(ps->peg_rules, main_rule);

  // expr = [ @id : id | expr @id : binop ]
  PegRule expr_rule = {.name = strdup("expr"), .seq = {.kind = PEG_SEQ}};
  expr_rule.seq.children = darray_new(sizeof(PegUnit), 0);

  PegUnit branches = {.kind = PEG_BRANCHES};
  branches.children = darray_new(sizeof(PegUnit), 0);

  PegUnit branch1 = {.kind = PEG_SEQ, .tag = strdup("id")};
  branch1.children = darray_new(sizeof(PegUnit), 0);
  PegUnit tok_id1 = {.kind = PEG_TOK, .name = strdup("id")};
  darray_push(branch1.children, tok_id1);
  darray_push(branches.children, branch1);

  PegUnit branch2 = {.kind = PEG_SEQ, .tag = strdup("binop")};
  branch2.children = darray_new(sizeof(PegUnit), 0);
  PegUnit self_ref = {.kind = PEG_ID, .name = strdup("expr")};
  darray_push(branch2.children, self_ref);
  PegUnit tok_id2 = {.kind = PEG_TOK, .name = strdup("id")};
  darray_push(branch2.children, tok_id2);
  darray_push(branches.children, branch2);

  darray_push(expr_rule.seq.children, branches);
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
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  // main = @id+ @num?
  PegRule main_rule = {.name = strdup("main"), .seq = {.kind = PEG_SEQ}};
  main_rule.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit tok1 = {.kind = PEG_TOK, .name = strdup("id"), .multiplier = '+'};
  darray_push(main_rule.seq.children, tok1);
  PegUnit tok2 = {.kind = PEG_TOK, .name = strdup("num"), .multiplier = '?'};
  darray_push(main_rule.seq.children, tok2);
  darray_push(ps->peg_rules, main_rule);

  bool ok = pp_detect_left_recursions(ps);
  assert(ok);

  parse_state_del(ps);
}

// ============================================================================
// pp_inline_macros: literal naming
// ============================================================================

TEST(test_inline_macros_literals) {
  ParseState* ps = parse_state_new();
  ps->vpa_rules = darray_new(sizeof(VpaRule), 0);

  // main = { /a/ @word *kw }
  VpaRule main_rule = {.name = strdup("main"), .is_scope = true};
  main_rule.units = darray_new(sizeof(VpaUnit), 0);
  VpaUnit word_u = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("word")};
  word_u.re = re_ir_emit_ch(word_u.re, 'a');
  darray_push(main_rule.units, word_u);
  VpaUnit kw_ref = {.kind = VPA_REF, .name = strdup("*kw")};
  darray_push(main_rule.units, kw_ref);
  darray_push(ps->vpa_rules, main_rule);

  // *kw = @{ "+" "-" }
  VpaRule kw_macro = {.name = strdup("kw"), .is_scope = true, .is_macro = true};
  kw_macro.units = darray_new(sizeof(VpaUnit), 0);
  VpaUnit plus_u = {.kind = VPA_REGEXP, .re = re_ir_new()};
  plus_u.re = re_ir_emit_ch(plus_u.re, '+');
  darray_push(kw_macro.units, plus_u);
  VpaUnit minus_u = {.kind = VPA_REGEXP, .re = re_ir_new()};
  minus_u.re = re_ir_emit_ch(minus_u.re, '-');
  darray_push(kw_macro.units, minus_u);
  darray_push(ps->vpa_rules, kw_macro);

  bool ok = pp_inline_macros(ps);
  assert(ok);

  // literals symtab should have lit.+ and lit.-
  assert(symtab_find(&ps->literals, "lit.+") >= 0);
  assert(symtab_find(&ps->literals, "lit.-") >= 0);

  // inlined units in main should have names
  VpaRule* mr = &ps->vpa_rules[0];
  bool found_plus = false;
  bool found_minus = false;
  for (int32_t j = 0; j < (int32_t)darray_size(mr->units); j++) {
    VpaUnit* u = &mr->units[j];
    if (u->kind == VPA_REGEXP && u->name) {
      if (strcmp(u->name, "lit.+") == 0) {
        found_plus = true;
      }
      if (strcmp(u->name, "lit.-") == 0) {
        found_minus = true;
      }
    }
  }
  assert(found_plus);
  assert(found_minus);

  parse_state_del(ps);
}

// ============================================================================
// pp_desugar_literal_tokens
// ============================================================================

TEST(test_desugar_literal_tokens) {
  ParseState* ps = parse_state_new();
  symtab_init(&ps->literals, 0);
  symtab_intern(&ps->literals, "lit.+");
  symtab_intern(&ps->literals, "lit.-");

  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  // expr = @num "+" @num
  PegRule rule = {.name = strdup("expr"), .seq = {.kind = PEG_SEQ}};
  rule.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t1 = {.kind = PEG_TOK, .name = strdup("num")};
  darray_push(rule.seq.children, t1);
  PegUnit kw = {.kind = PEG_KEYWORD_TOK, .name = strdup("+")};
  darray_push(rule.seq.children, kw);
  PegUnit t2 = {.kind = PEG_TOK, .name = strdup("num")};
  darray_push(rule.seq.children, t2);
  darray_push(ps->peg_rules, rule);

  bool ok = pp_desugar_literal_tokens(ps);
  assert(ok);

  PegUnit* desugared = &ps->peg_rules[0].seq.children[1];
  assert(desugared->kind == PEG_TOK);
  assert(strcmp(desugared->name, "lit.+") == 0);

  parse_state_del(ps);
}

TEST(test_desugar_literal_tokens_missing) {
  ParseState* ps = parse_state_new();
  symtab_init(&ps->literals, 0);

  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  PegRule rule = {.name = strdup("expr"), .seq = {.kind = PEG_SEQ}};
  rule.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit kw = {.kind = PEG_KEYWORD_TOK, .name = strdup("?")};
  darray_push(rule.seq.children, kw);
  darray_push(ps->peg_rules, rule);

  bool ok = pp_desugar_literal_tokens(ps);
  assert(!ok);
  assert(parse_has_error(ps));
  assert(strstr(parse_get_error(ps), "?") != NULL);

  parse_state_del(ps);
}

// ============================================================================
// pp_validate_vpa_scopes
// ============================================================================

TEST(test_validate_vpa_missing_main) {
  ParseState* ps = parse_state_new();
  ps->vpa_rules = darray_new(sizeof(VpaRule), 0);

  VpaRule rule = {.name = strdup("other"), .is_scope = true};
  rule.units = darray_new(sizeof(VpaUnit), 0);
  darray_push(ps->vpa_rules, rule);

  bool ok = pp_validate_vpa_scopes(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "main") != NULL);
  assert(strstr(parse_get_error(ps), "vpa") != NULL);

  parse_state_del(ps);
}

TEST(test_validate_vpa_duplicate_scope) {
  ParseState* ps = parse_state_new();
  ps->vpa_rules = darray_new(sizeof(VpaRule), 0);
  ps->effects = darray_new(sizeof(EffectDecl), 0);

  VpaRule main_r = {.name = strdup("main"), .is_scope = true};
  main_r.units = darray_new(sizeof(VpaUnit), 0);
  darray_push(ps->vpa_rules, main_r);

  VpaRule dup1 = {.name = strdup("foo"), .is_scope = true};
  dup1.units = darray_new(sizeof(VpaUnit), 0);
  VpaUnit begin1 = {.kind = VPA_REGEXP, .re = re_ir_new(), .hook = TOK_HOOK_BEGIN};
  begin1.re = re_ir_emit_ch(begin1.re, '(');
  darray_push(dup1.units, begin1);
  VpaUnit end1 = {.kind = VPA_REGEXP, .re = re_ir_new(), .hook = TOK_HOOK_END};
  end1.re = re_ir_emit_ch(end1.re, ')');
  darray_push(dup1.units, end1);
  darray_push(ps->vpa_rules, dup1);

  VpaRule dup2 = {.name = strdup("foo"), .is_scope = true};
  dup2.units = darray_new(sizeof(VpaUnit), 0);
  VpaUnit begin2 = {.kind = VPA_REGEXP, .re = re_ir_new(), .hook = TOK_HOOK_BEGIN};
  begin2.re = re_ir_emit_ch(begin2.re, '[');
  darray_push(dup2.units, begin2);
  VpaUnit end2 = {.kind = VPA_REGEXP, .re = re_ir_new(), .hook = TOK_HOOK_END};
  end2.re = re_ir_emit_ch(end2.re, ']');
  darray_push(dup2.units, end2);
  darray_push(ps->vpa_rules, dup2);

  bool ok = pp_validate_vpa_scopes(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "duplicate") != NULL);
  assert(strstr(parse_get_error(ps), "foo") != NULL);

  parse_state_del(ps);
}

TEST(test_validate_vpa_leading_re_empty) {
  ParseState* ps = parse_state_new();
  ps->vpa_rules = darray_new(sizeof(VpaRule), 0);
  ps->effects = darray_new(sizeof(EffectDecl), 0);

  VpaRule main_r = {.name = strdup("main"), .is_scope = true};
  main_r.units = darray_new(sizeof(VpaUnit), 0);
  darray_push(ps->vpa_rules, main_r);

  // scope with empty leading re
  VpaRule scope = {.name = strdup("str"), .is_scope = true};
  scope.units = darray_new(sizeof(VpaUnit), 0);
  VpaUnit leader = {.kind = VPA_REGEXP, .re = re_ir_new(), .hook = TOK_HOOK_BEGIN};
  darray_push(scope.units, leader);
  VpaUnit body_end = {.kind = VPA_REGEXP, .re = re_ir_new(), .hook = TOK_HOOK_END};
  body_end.re = re_ir_emit_ch(body_end.re, '"');
  darray_push(scope.units, body_end);
  darray_push(ps->vpa_rules, scope);

  bool ok = pp_validate_vpa_scopes(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "leading") != NULL);
  assert(strstr(parse_get_error(ps), "at least 1") != NULL);

  parse_state_del(ps);
}

TEST(test_validate_vpa_empty_re_needs_hook) {
  ParseState* ps = parse_state_new();
  ps->vpa_rules = darray_new(sizeof(VpaRule), 0);
  ps->effects = darray_new(sizeof(EffectDecl), 0);

  VpaRule main_r = {.name = strdup("main"), .is_scope = true};
  main_r.units = darray_new(sizeof(VpaUnit), 0);

  // empty fallback without .end or .fail
  VpaUnit fallback = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("bad")};
  darray_push(main_r.units, fallback);

  darray_push(ps->vpa_rules, main_r);

  bool ok = pp_validate_vpa_scopes(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "empty") != NULL);
  assert(strstr(parse_get_error(ps), ".end") != NULL);

  parse_state_del(ps);
}

TEST(test_validate_vpa_empty_re_with_end_ok) {
  ParseState* ps = parse_state_new();
  ps->vpa_rules = darray_new(sizeof(VpaRule), 0);
  ps->effects = darray_new(sizeof(EffectDecl), 0);

  VpaRule main_r = {.name = strdup("main"), .is_scope = true};
  main_r.units = darray_new(sizeof(VpaUnit), 0);

  VpaUnit tok = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("id")};
  tok.re = re_ir_emit_ch(tok.re, 'a');
  darray_push(main_r.units, tok);

  darray_push(ps->vpa_rules, main_r);

  bool ok = pp_validate_vpa_scopes(ps);
  assert(ok);

  parse_state_del(ps);
}

TEST(test_validate_vpa_two_empty_re) {
  ParseState* ps = parse_state_new();
  ps->vpa_rules = darray_new(sizeof(VpaRule), 0);
  ps->effects = darray_new(sizeof(EffectDecl), 0);

  VpaRule main_r = {.name = strdup("main"), .is_scope = true};
  main_r.units = darray_new(sizeof(VpaUnit), 0);

  VpaUnit e1 = {.kind = VPA_REGEXP, .re = re_ir_new(), .hook = TOK_HOOK_END};
  darray_push(main_r.units, e1);
  VpaUnit e2 = {.kind = VPA_REGEXP, .re = re_ir_new(), .hook = TOK_HOOK_FAIL};
  darray_push(main_r.units, e2);

  darray_push(ps->vpa_rules, main_r);

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
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  PegRule pr = {.name = strdup("helper"), .seq = {.kind = PEG_SEQ}};
  pr.seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(ps->peg_rules, pr);

  bool ok = pp_validate_peg_rules(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "main") != NULL);
  assert(strstr(parse_get_error(ps), "peg") != NULL);

  parse_state_del(ps);
}

TEST(test_validate_peg_duplicate_rule) {
  ParseState* ps = parse_state_new();
  ps->peg_rules = darray_new(sizeof(PegRule), 0);

  PegRule main_r = {.name = strdup("main"), .seq = {.kind = PEG_SEQ}};
  main_r.seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(ps->peg_rules, main_r);

  PegRule dup1 = {.name = strdup("expr"), .seq = {.kind = PEG_SEQ}};
  dup1.seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(ps->peg_rules, dup1);

  PegRule dup2 = {.name = strdup("expr"), .seq = {.kind = PEG_SEQ}};
  dup2.seq.children = darray_new(sizeof(PegUnit), 0);
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
  ps->vpa_rules = darray_new(sizeof(VpaRule), 0);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);
  ps->effects = darray_new(sizeof(EffectDecl), 0);
  ps->ignores.names = (Symtab){0};
  symtab_init(&ps->ignores.names, 0);

  // VPA main emits @id only
  VpaRule vr = {.name = strdup("main"), .is_scope = true};
  vr.units = darray_new(sizeof(VpaUnit), 0);
  VpaUnit u = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("id")};
  u.re = re_ir_emit_ch(u.re, 'a');
  darray_push(vr.units, u);
  darray_push(ps->vpa_rules, vr);

  // PEG main uses @id and @num (num not emitted by VPA)
  PegRule pr = {.name = strdup("main"), .seq = {.kind = PEG_SEQ}};
  pr.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t1 = {.kind = PEG_TOK, .name = strdup("id")};
  darray_push(pr.seq.children, t1);
  PegUnit t2 = {.kind = PEG_TOK, .name = strdup("num")};
  darray_push(pr.seq.children, t2);
  darray_push(ps->peg_rules, pr);

  bool ok = pp_match_scopes(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "num") != NULL);

  parse_state_del(ps);
}

TEST(test_match_scopes_ok) {
  ParseState* ps = parse_state_new();
  ps->vpa_rules = darray_new(sizeof(VpaRule), 0);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);
  ps->effects = darray_new(sizeof(EffectDecl), 0);
  ps->ignores.names = (Symtab){0};
  symtab_init(&ps->ignores.names, 0);

  // VPA main emits @id, @num, @space; %ignore @space
  VpaRule vr = {.name = strdup("main"), .is_scope = true};
  vr.units = darray_new(sizeof(VpaUnit), 0);
  VpaUnit u1 = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("id")};
  u1.re = re_ir_emit_ch(u1.re, 'a');
  darray_push(vr.units, u1);
  VpaUnit u2 = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("num")};
  u2.re = re_ir_emit_ch(u2.re, '0');
  darray_push(vr.units, u2);
  VpaUnit u3 = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("space")};
  u3.re = re_ir_emit_ch(u3.re, ' ');
  darray_push(vr.units, u3);
  darray_push(ps->vpa_rules, vr);

  symtab_intern(&ps->ignores.names, "space");

  // PEG main uses @id and @num
  PegRule pr = {.name = strdup("main"), .seq = {.kind = PEG_SEQ}};
  pr.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t1 = {.kind = PEG_TOK, .name = strdup("id")};
  darray_push(pr.seq.children, t1);
  PegUnit t2 = {.kind = PEG_TOK, .name = strdup("num")};
  darray_push(pr.seq.children, t2);
  darray_push(ps->peg_rules, pr);

  bool ok = pp_match_scopes(ps);
  assert(ok);

  parse_state_del(ps);
}

TEST(test_match_scopes_scope_ref_in_sets) {
  ParseState* ps = parse_state_new();
  ps->vpa_rules = darray_new(sizeof(VpaRule), 0);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);
  ps->effects = darray_new(sizeof(EffectDecl), 0);
  ps->ignores.names = (Symtab){0};
  symtab_init(&ps->ignores.names, 0);

  // VPA: foo = /.../ { @a ... }  (a scope)
  VpaRule foo_vr = {.name = strdup("foo"), .is_scope = true};
  foo_vr.units = darray_new(sizeof(VpaUnit), 0);
  VpaUnit foo_leader = {.kind = VPA_REGEXP, .re = re_ir_new(), .hook = TOK_HOOK_BEGIN};
  foo_leader.re = re_ir_emit_ch(foo_leader.re, '(');
  darray_push(foo_vr.units, foo_leader);
  VpaUnit foo_a = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("a")};
  foo_a.re = re_ir_emit_ch(foo_a.re, 'x');
  darray_push(foo_vr.units, foo_a);
  VpaUnit foo_end = {.kind = VPA_REGEXP, .re = re_ir_new(), .hook = TOK_HOOK_END};
  foo_end.re = re_ir_emit_ch(foo_end.re, ')');
  darray_push(foo_vr.units, foo_end);
  darray_push(ps->vpa_rules, foo_vr);

  // VPA: main = { @b foo }  (emit_set = {@b, foo})
  VpaRule main_vr = {.name = strdup("main"), .is_scope = true};
  main_vr.units = darray_new(sizeof(VpaUnit), 0);
  VpaUnit main_b = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("b")};
  main_b.re = re_ir_emit_ch(main_b.re, 'b');
  darray_push(main_vr.units, main_b);
  VpaUnit main_foo_ref = {.kind = VPA_REF, .name = strdup("foo")};
  darray_push(main_vr.units, main_foo_ref);
  darray_push(ps->vpa_rules, main_vr);

  // PEG: foo = @a  (foo is a scope, won't be expanded)
  PegRule foo_pr = {.name = strdup("foo"), .seq = {.kind = PEG_SEQ}};
  foo_pr.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit foo_tok = {.kind = PEG_TOK, .name = strdup("a")};
  darray_push(foo_pr.seq.children, foo_tok);
  darray_push(ps->peg_rules, foo_pr);

  // PEG: main = foo @b  (used_set = {foo, @b})
  PegRule main_pr = {.name = strdup("main"), .seq = {.kind = PEG_SEQ}};
  main_pr.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit main_foo = {.kind = PEG_ID, .name = strdup("foo")};
  darray_push(main_pr.seq.children, main_foo);
  PegUnit main_tok = {.kind = PEG_TOK, .name = strdup("b")};
  darray_push(main_pr.seq.children, main_tok);
  darray_push(ps->peg_rules, main_pr);

  bool ok = pp_match_scopes(ps);
  assert(ok);

  parse_state_del(ps);
}

TEST(test_match_scopes_scope_ref_mismatch) {
  ParseState* ps = parse_state_new();
  ps->vpa_rules = darray_new(sizeof(VpaRule), 0);
  ps->peg_rules = darray_new(sizeof(PegRule), 0);
  ps->effects = darray_new(sizeof(EffectDecl), 0);
  ps->ignores.names = (Symtab){0};
  symtab_init(&ps->ignores.names, 0);

  // VPA: foo = /.../ { @a ... }  (a scope)
  VpaRule foo_vr = {.name = strdup("foo"), .is_scope = true};
  foo_vr.units = darray_new(sizeof(VpaUnit), 0);
  VpaUnit foo_leader = {.kind = VPA_REGEXP, .re = re_ir_new(), .hook = TOK_HOOK_BEGIN};
  foo_leader.re = re_ir_emit_ch(foo_leader.re, '(');
  darray_push(foo_vr.units, foo_leader);
  VpaUnit foo_a = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("a")};
  foo_a.re = re_ir_emit_ch(foo_a.re, 'x');
  darray_push(foo_vr.units, foo_a);
  VpaUnit foo_end = {.kind = VPA_REGEXP, .re = re_ir_new(), .hook = TOK_HOOK_END};
  foo_end.re = re_ir_emit_ch(foo_end.re, ')');
  darray_push(foo_vr.units, foo_end);
  darray_push(ps->vpa_rules, foo_vr);

  // VPA: main = { @b foo }  (emit_set = {@b, foo})
  VpaRule main_vr = {.name = strdup("main"), .is_scope = true};
  main_vr.units = darray_new(sizeof(VpaUnit), 0);
  VpaUnit main_b = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("b")};
  main_b.re = re_ir_emit_ch(main_b.re, 'b');
  darray_push(main_vr.units, main_b);
  VpaUnit main_foo_ref = {.kind = VPA_REF, .name = strdup("foo")};
  darray_push(main_vr.units, main_foo_ref);
  darray_push(ps->vpa_rules, main_vr);

  // PEG: main = @b  (used_set = {@b}, missing foo)
  PegRule main_pr = {.name = strdup("main"), .seq = {.kind = PEG_SEQ}};
  main_pr.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit main_tok = {.kind = PEG_TOK, .name = strdup("b")};
  darray_push(main_pr.seq.children, main_tok);
  darray_push(ps->peg_rules, main_pr);

  bool ok = pp_match_scopes(ps);
  assert(!ok);
  assert(strstr(parse_get_error(ps), "foo") != NULL);

  parse_state_del(ps);
}

int main(void) {
  printf("test_post_process:\n");

  RUN(test_inline_macros);
  RUN(test_inline_macros_missing);
  RUN(test_inline_macros_literals);
  RUN(test_auto_tag_branches);
  RUN(test_duplicate_tag_error);
  RUN(test_no_duplicate_tags);
  RUN(test_detect_left_recursion);
  RUN(test_no_left_recursion);
  RUN(test_desugar_literal_tokens);
  RUN(test_desugar_literal_tokens_missing);
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

  printf("all ok\n");
  return 0;
}
