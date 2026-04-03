#include "../src/darray.h"
#include "../src/header_writer.h"
#include "../src/irwriter.h"
#include "../src/parse.h"
#include "../src/token_tree.h"
#include "../src/ustr.h"
#include "../src/vpa.h"
#include "compat.h"
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

// === TokenTree / TokenChunk tests ===

TEST(test_tree_new_del) {
  char* ustr = ustr_new(5, "hello");
  assert(ustr != NULL);
  TokenTree* tree = tc_tree_new(ustr);
  assert(tree != NULL);
  assert(tree->src == ustr);
  assert(tree->root != NULL);
  assert(tree->current == tree->root);
  tc_tree_del(tree);
  ustr_del(ustr);
}

TEST(test_tree_add_token) {
  char* ustr = ustr_new(3, "abc");
  TokenTree* tree = tc_tree_new(ustr);

  tc_add(tree, TOK_CHAR, 0, 1, -1);
  tc_add(tree, TOK_CHAR, 1, 1, -1);

  assert(darray_size(tree->current->tokens) == 2);
  assert(tree->current->tokens[0].tok_id == TOK_CHAR);
  assert(tree->current->tokens[0].cp_start == 0);
  assert(tree->current->tokens[1].cp_start == 1);

  tc_tree_del(tree);
  ustr_del(ustr);
}

TEST(test_tree_push_pop) {
  char* ustr = ustr_new(5, "ab\ncd");
  TokenTree* tree = tc_tree_new(ustr);

  TokenChunk* root = tree->current;
  assert(root->parent_id == -1);

  // push child scope
  TokenChunk* child = tc_push(tree, 0);
  assert(child != NULL);
  assert(tree->current == child);
  assert(child->parent_id != -1);

  // add token in child
  tc_add(tree, TOK_VPA_ID, 0, 2, -1);
  assert(darray_size(child->tokens) == 1);

  // pop back to root
  TokenChunk* popped = tc_pop(tree);
  assert(popped != NULL);
  assert(tree->current == root);

  tc_tree_del(tree);
  ustr_del(ustr);
}

TEST(test_tree_nested_push_pop) {
  char* ustr = ustr_new(1, "x");
  TokenTree* tree = tc_tree_new(ustr);

  TokenChunk* root = tree->current;

  // push level 1
  TokenChunk* l1 = tc_push(tree, 0);
  assert(tree->current == l1);

  // push level 2
  TokenChunk* l2 = tc_push(tree, 0);
  assert(tree->current == l2);

  // pop level 2 -> level 1
  tc_pop(tree);
  assert(tree->current == l1);

  // pop level 1 -> root
  tc_pop(tree);
  assert(tree->current == root);

  tc_tree_del(tree);
  ustr_del(ustr);
}

TEST(test_tree_locate_single_line) {
  char* ustr = ustr_new(5, "hello");
  TokenTree* tree = tc_tree_new(ustr);

  // feed newline bitmap (no newlines in "hello")
  Location loc = tc_locate(tree, 0);
  assert(loc.line == 0);
  assert(loc.col == 0);

  loc = tc_locate(tree, 3);
  assert(loc.line == 0);
  assert(loc.col == 3);

  tc_tree_del(tree);
  ustr_del(ustr);
}

TEST(test_tree_locate_multiline) {
  // "ab\ncd\ne" = 7 codepoints
  char* ustr = ustr_new(7, "ab\ncd\ne");
  TokenTree* tree = tc_tree_new(ustr);

  // mark newlines in the bitmap: cp_offset 2 and 5 are '\n'
  // newline_map is already allocated by tc_tree_new via calloc
  tree->newline_map[0] |= (1ULL << 2);
  tree->newline_map[0] |= (1ULL << 5);

  Location loc;
  // 'a' at offset 0 -> line 0, col 0
  loc = tc_locate(tree, 0);
  assert(loc.line == 0);
  assert(loc.col == 0);

  // 'b' at offset 1 -> line 0, col 1
  loc = tc_locate(tree, 1);
  assert(loc.line == 0);
  assert(loc.col == 1);

  // 'c' at offset 3 -> line 1, col 0
  loc = tc_locate(tree, 3);
  assert(loc.line == 1);
  assert(loc.col == 0);

  // 'd' at offset 4 -> line 1, col 1
  loc = tc_locate(tree, 4);
  assert(loc.line == 1);
  assert(loc.col == 1);

  // 'e' at offset 6 -> line 2, col 0
  loc = tc_locate(tree, 6);
  assert(loc.line == 2);
  assert(loc.col == 0);

  tc_tree_del(tree);
  ustr_del(ustr);
}

// === Token struct layout ===

TEST(test_token_size) { assert(sizeof(Token) == 16); }

// === VpaUnit construction ===

TEST(test_vpa_unit_regexp) {
  VpaUnit u = {0};
  u.kind = VPA_REGEXP;
  u.re = re_ir_new();
  u.re = re_ir_emit_ch(u.re, 'a');
  u.name = strdup("my_token");
  u.hook = 0;
  u.user_hook = NULL;
  u.children = NULL;

  assert(u.kind == VPA_REGEXP);
  assert(darray_size(u.re) == 1);
  assert(strcmp(u.name, "my_token") == 0);
  assert(u.hook == 0);

  re_ir_free(u.re);
  free(u.name);
}

TEST(test_vpa_unit_ref) {
  VpaUnit u = {0};
  u.kind = VPA_REF;
  u.name = strdup("child_scope");

  assert(u.kind == VPA_REF);
  assert(strcmp(u.name, "child_scope") == 0);

  free(u.name);
}

TEST(test_vpa_unit_scope) {
  VpaUnit u = {0};
  u.kind = VPA_SCOPE;
  u.name = strdup("braced");
  u.hook = TOK_HOOK_BEGIN;
  u.children = darray_new(sizeof(VpaUnit), 0);

  // add a child regexp unit
  VpaUnit child = {0};
  child.kind = VPA_REGEXP;
  child.re = re_ir_new();
  child.re = re_ir_emit_ch(child.re, 'x');
  child.name = strdup("inner_tok");
  darray_push(u.children, child);

  assert(u.kind == VPA_SCOPE);
  assert(darray_size(u.children) == 1);
  assert(u.children[0].kind == VPA_REGEXP);

  re_ir_free(u.children[0].re);
  free(u.children[0].name);
  darray_del(u.children);
  free(u.name);
}

TEST(test_vpa_unit_hooks) {
  VpaUnit u = {0};
  u.kind = VPA_REGEXP;
  u.hook = TOK_HOOK_BEGIN;
  assert(u.hook == TOK_HOOK_BEGIN);

  u.hook = TOK_HOOK_END;
  assert(u.hook == TOK_HOOK_END);

  u.hook = TOK_HOOK_FAIL;
  assert(u.hook == TOK_HOOK_FAIL);

  u.hook = TOK_HOOK_UNPARSE;
  assert(u.hook == TOK_HOOK_UNPARSE);

  u.user_hook = strdup("my_custom_hook");
  assert(strcmp(u.user_hook, "my_custom_hook") == 0);
  free(u.user_hook);
}

// === VpaRule construction ===

TEST(test_vpa_rule_basic) {
  VpaRule rule = {0};
  rule.name = strdup("main");
  rule.units = darray_new(sizeof(VpaUnit), 0);
  rule.is_scope = true;
  rule.is_macro = false;

  VpaUnit u = {0};
  u.kind = VPA_REGEXP;
  u.re = re_ir_new();
  u.re = re_ir_emit_ch(u.re, 'a');
  u.name = strdup("tok_a");
  darray_push(rule.units, u);

  assert(strcmp(rule.name, "main") == 0);
  assert(rule.is_scope == true);
  assert(rule.is_macro == false);
  assert(darray_size(rule.units) == 1);
  assert(rule.units[0].kind == VPA_REGEXP);

  re_ir_free(rule.units[0].re);
  free(rule.units[0].name);
  darray_del(rule.units);
  free(rule.name);
}

TEST(test_vpa_rule_macro) {
  VpaRule rule = {0};
  rule.name = strdup("_helper");
  rule.units = darray_new(sizeof(VpaUnit), 0);
  rule.is_scope = false;
  rule.is_macro = true;

  assert(rule.is_macro == true);
  assert(rule.is_scope == false);

  darray_del(rule.units);
  free(rule.name);
}

// === KeywordEntry ===

TEST(test_keyword_entry) {
  KeywordEntry kw = {0};
  kw.group = strdup("type_keywords");
  kw.lit_off = 10;
  kw.lit_len = 3;
  kw.src = "some source text int more";

  assert(strcmp(kw.group, "type_keywords") == 0);
  assert(kw.lit_off == 10);
  assert(kw.lit_len == 3);
  assert(memcmp(kw.src + kw.lit_off, "e t", kw.lit_len) == 0);

  free(kw.group);
}

// === EffectDecl ===

TEST(test_effect_decl) {
  EffectDecl ed = {0};
  ed.hook_name = strdup("on_open_brace");
  ed.effects = darray_new(sizeof(int32_t), 0);

  int32_t e1 = 1, e2 = 2;
  darray_push(ed.effects, e1);
  darray_push(ed.effects, e2);

  assert(strcmp(ed.hook_name, "on_open_brace") == 0);
  assert(darray_size(ed.effects) == 2);
  assert(ed.effects[0] == 1);
  assert(ed.effects[1] == 2);

  darray_del(ed.effects);
  free(ed.hook_name);
}

// === IgnoreSet ===

TEST(test_ignore_set) {
  IgnoreSet ig = {0};
  ig.names = darray_new(sizeof(char*), 0);

  char* n1 = strdup("whitespace");
  char* n2 = strdup("comment");
  darray_push(ig.names, n1);
  darray_push(ig.names, n2);

  assert(darray_size(ig.names) == 2);
  assert(strcmp(ig.names[0], "whitespace") == 0);
  assert(strcmp(ig.names[1], "comment") == 0);

  free(ig.names[0]);
  free(ig.names[1]);
  darray_del(ig.names);
}

// === VpaGenInput assembly ===

TEST(test_vpa_gen_input_empty) {
  VpaGenInput input = {0};
  input.rules = darray_new(sizeof(VpaRule), 0);
  input.keywords = darray_new(sizeof(KeywordEntry), 0);
  input.effects = darray_new(sizeof(EffectDecl), 0);
  input.peg_rules = darray_new(sizeof(PegRule), 0);
  input.src = "source";

  assert(darray_size(input.rules) == 0);
  assert(darray_size(input.keywords) == 0);
  assert(darray_size(input.effects) == 0);
  assert(darray_size(input.peg_rules) == 0);

  darray_del(input.rules);
  darray_del(input.keywords);
  darray_del(input.effects);
  darray_del(input.peg_rules);
}

TEST(test_vpa_gen_input_with_rules) {
  VpaGenInput input = {0};
  input.rules = darray_new(sizeof(VpaRule), 0);
  input.keywords = darray_new(sizeof(KeywordEntry), 0);
  input.effects = darray_new(sizeof(EffectDecl), 0);
  input.peg_rules = darray_new(sizeof(PegRule), 0);
  input.src = "test source";

  // add main rule with a regexp unit
  VpaRule main_rule = {0};
  main_rule.name = strdup("main");
  main_rule.units = darray_new(sizeof(VpaUnit), 0);
  main_rule.is_scope = true;

  VpaUnit u = {0};
  u.kind = VPA_REGEXP;
  u.re = re_ir_new();
  u.re = re_ir_emit_ch(u.re, '/');
  u.name = strdup("slash");
  darray_push(main_rule.units, u);

  darray_push(input.rules, main_rule);

  assert(darray_size(input.rules) == 1);
  assert(strcmp(input.rules[0].name, "main") == 0);
  assert(darray_size(input.rules[0].units) == 1);

  // cleanup
  re_ir_free(input.rules[0].units[0].re);
  free(input.rules[0].units[0].name);
  darray_del(input.rules[0].units);
  free(input.rules[0].name);
  darray_del(input.rules);
  darray_del(input.keywords);
  darray_del(input.effects);
  darray_del(input.peg_rules);
}

// === Scope handling: simulating the spec's nested scope example ===

TEST(test_scope_handling) {
  // Simulate the spec example:
  //   s = /regex1/ { /regex2/  a }
  //   a = /regex3/ .hook1 { /regex4/ }

  VpaRule rule_s = {0};
  rule_s.name = strdup("s");
  rule_s.units = darray_new(sizeof(VpaUnit), 0);
  rule_s.is_scope = true;

  // leader: /regex1/
  VpaUnit leader_s = {0};
  leader_s.kind = VPA_REGEXP;
  leader_s.re = re_ir_new();
  leader_s.re = re_ir_emit_ch(leader_s.re, 'r');
  leader_s.name = strdup("s_leader");
  darray_push(rule_s.units, leader_s);

  // scope body
  VpaUnit scope_s = {0};
  scope_s.kind = VPA_SCOPE;
  scope_s.name = strdup("s_body");
  scope_s.hook = TOK_HOOK_BEGIN;
  scope_s.children = darray_new(sizeof(VpaUnit), 0);

  // /regex2/ in scope body
  VpaUnit regex2 = {0};
  regex2.kind = VPA_REGEXP;
  regex2.re = re_ir_new();
  regex2.re = re_ir_emit_ch(regex2.re, 'x');
  regex2.name = strdup("tok_regex2");
  darray_push(scope_s.children, regex2);

  // ref to 'a'
  VpaUnit ref_a = {0};
  ref_a.kind = VPA_REF;
  ref_a.name = strdup("a");
  darray_push(scope_s.children, ref_a);

  darray_push(rule_s.units, scope_s);

  // rule 'a'
  VpaRule rule_a = {0};
  rule_a.name = strdup("a");
  rule_a.units = darray_new(sizeof(VpaUnit), 0);
  rule_a.is_scope = true;

  // leader: /regex3/ with .hook1
  VpaUnit leader_a = {0};
  leader_a.kind = VPA_REGEXP;
  leader_a.re = re_ir_new();
  leader_a.re = re_ir_emit_ch(leader_a.re, 'y');
  leader_a.name = strdup("a_leader");
  leader_a.user_hook = strdup("hook1");
  darray_push(rule_a.units, leader_a);

  // scope body of 'a'
  VpaUnit scope_a = {0};
  scope_a.kind = VPA_SCOPE;
  scope_a.name = strdup("a_body");
  scope_a.hook = TOK_HOOK_BEGIN;
  scope_a.children = darray_new(sizeof(VpaUnit), 0);

  VpaUnit regex4 = {0};
  regex4.kind = VPA_REGEXP;
  regex4.re = re_ir_new();
  regex4.re = re_ir_emit_ch(regex4.re, 'z');
  regex4.name = strdup("tok_regex4");
  darray_push(scope_a.children, regex4);

  darray_push(rule_a.units, scope_a);

  // verify structure
  assert(darray_size(rule_s.units) == 2);
  assert(rule_s.units[0].kind == VPA_REGEXP);
  assert(rule_s.units[1].kind == VPA_SCOPE);
  assert(darray_size(rule_s.units[1].children) == 2);
  assert(rule_s.units[1].children[0].kind == VPA_REGEXP);
  assert(rule_s.units[1].children[1].kind == VPA_REF);
  assert(strcmp(rule_s.units[1].children[1].name, "a") == 0);

  assert(darray_size(rule_a.units) == 2);
  assert(rule_a.units[0].kind == VPA_REGEXP);
  assert(strcmp(rule_a.units[0].user_hook, "hook1") == 0);
  assert(rule_a.units[1].kind == VPA_SCOPE);
  assert(darray_size(rule_a.units[1].children) == 1);

  // cleanup rule_s
  re_ir_free(rule_s.units[0].re);
  free(rule_s.units[0].name);
  re_ir_free(rule_s.units[1].children[0].re);
  free(rule_s.units[1].children[0].name);
  free(rule_s.units[1].children[1].name);
  darray_del(rule_s.units[1].children);
  free(rule_s.units[1].name);
  darray_del(rule_s.units);
  free(rule_s.name);

  // cleanup rule_a
  re_ir_free(rule_a.units[0].re);
  free(rule_a.units[0].name);
  free(rule_a.units[0].user_hook);
  re_ir_free(rule_a.units[1].children[0].re);
  free(rule_a.units[1].children[0].name);
  darray_del(rule_a.units[1].children);
  free(rule_a.units[1].name);
  darray_del(rule_a.units);
  free(rule_a.name);
}

// === ReIr for VPA units ===

TEST(test_re_ir_for_vpa) {
  ReIr ir = re_ir_new();
  assert(darray_size(ir) == 0);

  // build a simple pattern: (a|b)
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0);
  ir = re_ir_emit_ch(ir, 'a');
  ir = re_ir_emit(ir, RE_IR_FORK, 0, 0);
  ir = re_ir_emit_ch(ir, 'b');
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0);

  assert(darray_size(ir) == 5);
  assert(ir[0].kind == RE_IR_LPAREN);
  assert(ir[1].kind == RE_IR_APPEND_CH);
  assert(ir[2].kind == RE_IR_FORK);
  assert(ir[3].kind == RE_IR_APPEND_CH);
  assert(ir[4].kind == RE_IR_RPAREN);

  ReIr clone = re_ir_clone(ir);
  assert(darray_size(clone) == 5);
  assert(clone[0].kind == RE_IR_LPAREN);

  re_ir_free(ir);
  re_ir_free(clone);
}

TEST(test_re_ir_build_literal) {
  char* ustr = ustr_new(3, "abc");
  assert(ustr != NULL);

  ReIr ir = re_ir_build_literal(ustr, 0, 3);
  assert(ir != NULL);
  assert(darray_size(ir) == 3);
  assert(ir[0].kind == RE_IR_APPEND_CH);
  assert(ir[1].kind == RE_IR_APPEND_CH);
  assert(ir[2].kind == RE_IR_APPEND_CH);

  re_ir_free(ir);
  ustr_del(ustr);
}

// === Token chunk scope simulation ===

TEST(test_chunk_scope_ids) {
  char* ustr = ustr_new(1, "x");
  TokenTree* tree = tc_tree_new(ustr);

  tree->root->scope_id = SCOPE_MAIN;

  tc_push(tree, SCOPE_VPA);

  assert(tree->current->scope_id == SCOPE_VPA);

  tc_push(tree, SCOPE_RE);
  assert(tree->current->scope_id == SCOPE_RE);

  tc_pop(tree);
  assert(tree->current->scope_id == SCOPE_VPA);

  tc_pop(tree);
  assert(tree->current->scope_id == SCOPE_MAIN);

  tc_tree_del(tree);
  ustr_del(ustr);
}

// === Token with scope reference (chunk_id) ===

TEST(test_token_scope_reference) {
  char* ustr = ustr_new(3, "abc");
  TokenTree* tree = tc_tree_new(ustr);

  // push a child chunk, remember its index
  tc_push(tree, SCOPE_VPA);
  int32_t child_chunk_id = (int32_t)(darray_size(tree->table) - 1);

  // pop back
  tc_pop(tree);

  // add a token in root that references the child chunk
  tc_add(tree, SCOPE_VPA, 0, 3, child_chunk_id);

  assert(darray_size(tree->root->tokens) == 1);
  Token stored = tree->root->tokens[0];
  assert(stored.tok_id == SCOPE_VPA);
  assert(stored.tok_id < SCOPE_COUNT); // it is a scope reference
  assert(stored.chunk_id == child_chunk_id);

  // the referenced chunk should be in the table
  assert(tree->table[stored.chunk_id].scope_id == SCOPE_VPA);

  tc_tree_del(tree);
  ustr_del(ustr);
}

// === vpa_gen tests ===

static void _free_gen_input(VpaGenInput* input) {
  for (int32_t i = 0; i < (int32_t)darray_size(input->rules); i++) {
    VpaRule* rule = &input->rules[i];
    for (int32_t j = 0; j < (int32_t)darray_size(rule->units); j++) {
      VpaUnit* u = &rule->units[j];
      re_ir_free(u->re);
      free(u->name);
      free(u->user_hook);
      for (int32_t k = 0; k < (int32_t)darray_size(u->children); k++) {
        re_ir_free(u->children[k].re);
        free(u->children[k].name);
        free(u->children[k].user_hook);
        darray_del(u->children[k].children);
      }
      darray_del(u->children);
    }
    darray_del(rule->units);
    free(rule->name);
  }
  darray_del(input->rules);
  darray_del(input->keywords);
  for (int32_t i = 0; i < (int32_t)darray_size(input->effects); i++) {
    free(input->effects[i].hook_name);
    darray_del(input->effects[i].effects);
  }
  darray_del(input->effects);
  darray_del(input->peg_rules);
}

static void _compile_test(const char* h_file, const char* ir_file) {
  const char* null_out = compat_devnull_path();
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "%s -c -x c %s -o %s 2>&1", compat_llvm_cc(), h_file, null_out);
  int ret = system(cmd);
  assert(ret == 0);

  snprintf(cmd, sizeof(cmd), "%s -c %s -o %s 2>&1", compat_llvm_cc(), ir_file, null_out);
  ret = system(cmd);
  assert(ret == 0);
}

TEST(test_vpa_gen_empty_rules) {
  VpaGenInput input = {0};
  input.rules = darray_new(sizeof(VpaRule), 0);
  input.keywords = darray_new(sizeof(KeywordEntry), 0);
  input.effects = darray_new(sizeof(EffectDecl), 0);
  input.peg_rules = darray_new(sizeof(PegRule), 0);
  input.src = "";

  FILE* hf = fopen(BUILD_DIR "/test_vpa_gen_empty.h", "w");
  FILE* irf = fopen(BUILD_DIR "/test_vpa_gen_empty.ll", "w");
  assert(hf && irf);
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test_vpa.c", ".");
  vpa_gen(&input, hw, w);
  irwriter_end(w);

  hw_del(hw);
  irwriter_del(w);
  fclose(irf);
  fclose(hf);

  _free_gen_input(&input);
}

TEST(test_vpa_gen_single_scope) {
  VpaGenInput input = {0};
  input.rules = darray_new(sizeof(VpaRule), 0);
  input.keywords = darray_new(sizeof(KeywordEntry), 0);
  input.effects = darray_new(sizeof(EffectDecl), 0);
  input.peg_rules = darray_new(sizeof(PegRule), 0);
  input.src = "";

  VpaRule main_rule = {0};
  main_rule.name = strdup("main");
  main_rule.units = darray_new(sizeof(VpaUnit), 0);
  main_rule.is_scope = true;

  VpaUnit u1 = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("tok_a")};
  u1.re = re_ir_emit_ch(u1.re, 'a');
  darray_push(main_rule.units, u1);

  VpaUnit u2 = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("tok_b")};
  u2.re = re_ir_emit_ch(u2.re, 'b');
  darray_push(main_rule.units, u2);

  darray_push(input.rules, main_rule);

  FILE* hf = fopen(BUILD_DIR "/test_vpa_gen_scope.h", "w");
  FILE* irf = fopen(BUILD_DIR "/test_vpa_gen_scope.ll", "w");
  assert(hf && irf);
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test_vpa.c", ".");
  vpa_gen(&input, hw, w);
  irwriter_end(w);

  hw_del(hw);
  irwriter_del(w);
  fclose(irf);
  fclose(hf);

  char* ir_buf;
  size_t ir_sz;
  FILE* ir_read = fopen(BUILD_DIR "/test_vpa_gen_scope.ll", "r");
  assert(ir_read);
  fseek(ir_read, 0, SEEK_END);
  ir_sz = (size_t)ftell(ir_read);
  rewind(ir_read);
  ir_buf = malloc(ir_sz + 1);
  fread(ir_buf, 1, ir_sz, ir_read);
  ir_buf[ir_sz] = '\0';
  fclose(ir_read);

  assert(strstr(ir_buf, "define {i64, i64} @lex_main") != NULL);
  assert(strstr(ir_buf, "@vpa_dispatch") != NULL);
  assert(strstr(ir_buf, "@vpa_lex") != NULL);
  free(ir_buf);

  char* h_buf;
  size_t h_sz;
  FILE* h_read = fopen(BUILD_DIR "/test_vpa_gen_scope.h", "r");
  assert(h_read);
  fseek(h_read, 0, SEEK_END);
  h_sz = (size_t)ftell(h_read);
  rewind(h_read);
  h_buf = malloc(h_sz + 1);
  fread(h_buf, 1, h_sz, h_read);
  h_buf[h_sz] = '\0';
  fclose(h_read);

  assert(strstr(h_buf, "SCOPE_MAIN") != NULL);
  assert(strstr(h_buf, "TOK_TOK_A") != NULL);
  assert(strstr(h_buf, "TOK_TOK_B") != NULL);
  assert(strstr(h_buf, "tc_tree_new") != NULL);
  free(h_buf);

  _compile_test(BUILD_DIR "/test_vpa_gen_scope.h", BUILD_DIR "/test_vpa_gen_scope.ll");

  _free_gen_input(&input);
}

TEST(test_vpa_gen_multi_scope) {
  VpaGenInput input = {0};
  input.rules = darray_new(sizeof(VpaRule), 0);
  input.keywords = darray_new(sizeof(KeywordEntry), 0);
  input.effects = darray_new(sizeof(EffectDecl), 0);
  input.peg_rules = darray_new(sizeof(PegRule), 0);
  input.src = "";

  VpaRule main_rule = {0};
  main_rule.name = strdup("main");
  main_rule.units = darray_new(sizeof(VpaUnit), 0);
  main_rule.is_scope = true;

  VpaUnit u1 = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("tok_x")};
  u1.re = re_ir_emit_ch(u1.re, 'x');
  darray_push(main_rule.units, u1);

  VpaUnit ref = {.kind = VPA_REF, .name = strdup("inner")};
  darray_push(main_rule.units, ref);

  darray_push(input.rules, main_rule);

  VpaRule inner_rule = {0};
  inner_rule.name = strdup("inner");
  inner_rule.units = darray_new(sizeof(VpaUnit), 0);
  inner_rule.is_scope = false;

  VpaUnit leader = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("open")};
  leader.re = re_ir_emit_ch(leader.re, '{');
  darray_push(inner_rule.units, leader);

  VpaUnit scope_body = {.kind = VPA_SCOPE, .name = strdup("inner_body"), .hook = TOK_HOOK_BEGIN};
  scope_body.re = re_ir_new();
  scope_body.re = re_ir_emit_ch(scope_body.re, '{');
  scope_body.children = darray_new(sizeof(VpaUnit), 0);

  VpaUnit inner_tok = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("tok_y")};
  inner_tok.re = re_ir_emit_ch(inner_tok.re, 'y');
  darray_push(scope_body.children, inner_tok);

  darray_push(inner_rule.units, scope_body);
  darray_push(input.rules, inner_rule);

  FILE* hf = fopen(BUILD_DIR "/test_vpa_gen_multi.h", "w");
  FILE* irf = fopen(BUILD_DIR "/test_vpa_gen_multi.ll", "w");
  assert(hf && irf);
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test_vpa.c", ".");
  vpa_gen(&input, hw, w);
  irwriter_end(w);

  hw_del(hw);
  irwriter_del(w);
  fclose(irf);
  fclose(hf);

  char* ir_buf;
  size_t ir_sz;
  FILE* ir_read = fopen(BUILD_DIR "/test_vpa_gen_multi.ll", "r");
  assert(ir_read);
  fseek(ir_read, 0, SEEK_END);
  ir_sz = (size_t)ftell(ir_read);
  rewind(ir_read);
  ir_buf = malloc(ir_sz + 1);
  fread(ir_buf, 1, ir_sz, ir_read);
  ir_buf[ir_sz] = '\0';
  fclose(ir_read);

  assert(strstr(ir_buf, "lex_main") != NULL);
  assert(strstr(ir_buf, "lex_inner") != NULL);
  free(ir_buf);

  char* h_buf;
  size_t h_sz;
  FILE* h_read = fopen(BUILD_DIR "/test_vpa_gen_multi.h", "r");
  assert(h_read);
  fseek(h_read, 0, SEEK_END);
  h_sz = (size_t)ftell(h_read);
  rewind(h_read);
  h_buf = malloc(h_sz + 1);
  fread(h_buf, 1, h_sz, h_read);
  h_buf[h_sz] = '\0';
  fclose(h_read);

  assert(strstr(h_buf, "SCOPE_MAIN") != NULL);
  assert(strstr(h_buf, "SCOPE_INNER") != NULL);
  free(h_buf);

  _compile_test(BUILD_DIR "/test_vpa_gen_multi.h", BUILD_DIR "/test_vpa_gen_multi.ll");

  _free_gen_input(&input);
}

// === Binary mode flag ===

TEST(test_vpa_unit_binary_mode) {
  VpaUnit u = {0};
  u.kind = VPA_REGEXP;
  u.binary_mode = true;
  u.name = strdup("bin_tok");
  u.re = re_ir_new();

  assert(u.binary_mode == true);

  u.binary_mode = false;
  assert(u.binary_mode == false);

  re_ir_free(u.re);
  free(u.name);
}

// === Execution test: compile + link + run generated VPA lexer ===

TEST(test_vpa_gen_exec) {
  const char* driver_path = BUILD_DIR "/test_vpa_exec_driver.c";
  FILE* df = fopen(driver_path, "w");
  assert(df);
  fprintf(df, "#include <assert.h>\n"
              "#include <stdint.h>\n"
              "#include <string.h>\n"
              "#define NEST_RT_IMPLEMENTATION\n"
              "#include \"test_vpa_gen_scope.h\"\n"
              "\n"
              "int32_t vpa_rt_read_cp(void* src, int32_t cp_off) {\n"
              "  return ((const unsigned char*)src)[cp_off];\n"
              "}\n"
              "\n"
              "int main(void) {\n"
              "  const char* input = \"aabb\";\n"
              "  int32_t len = 4;\n"
              "  char* us = ustr_new(len, input);\n"
              "  TokenTree* tt = tc_tree_new(us);\n"
              "  vpa_lex((int64_t)(intptr_t)input, (int64_t)len, (int64_t)(intptr_t)tt);\n"
              "  int32_t n = (int32_t)darray_size(tt->root->tokens);\n"
              "  assert(n == 4);\n"
              "  assert(tt->root->tokens[0].tok_id == TOK_TOK_A);\n"
              "  assert(tt->root->tokens[1].tok_id == TOK_TOK_A);\n"
              "  assert(tt->root->tokens[2].tok_id == TOK_TOK_B);\n"
              "  assert(tt->root->tokens[3].tok_id == TOK_TOK_B);\n"
              "  assert(tt->root->tokens[0].cp_start == 0);\n"
              "  assert(tt->root->tokens[0].cp_size == 1);\n"
              "  assert(tt->root->tokens[3].cp_start == 3);\n"
              "  tc_tree_del(tt);\n"
              "  ustr_del(us);\n"
              "  return 0;\n"
              "}\n");
  fclose(df);

  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "%s %s %s -o %s 2>&1", compat_llvm_cc(), driver_path, BUILD_DIR "/test_vpa_gen_scope.ll",
           BUILD_DIR "/test_vpa_exec");
  int ret = system(cmd);
  assert(ret == 0);

  snprintf(cmd, sizeof(cmd), "%s", BUILD_DIR "/test_vpa_exec");
  ret = system(cmd);
  assert(ret == 0);
}

int main(void) {
  printf("test_vpa:\n");

  RUN(test_tree_new_del);
  RUN(test_tree_add_token);
  RUN(test_tree_push_pop);
  RUN(test_tree_nested_push_pop);
  RUN(test_tree_locate_single_line);
  RUN(test_tree_locate_multiline);
  RUN(test_token_size);

  RUN(test_vpa_unit_regexp);
  RUN(test_vpa_unit_ref);
  RUN(test_vpa_unit_scope);
  RUN(test_vpa_unit_hooks);

  RUN(test_vpa_rule_basic);
  RUN(test_vpa_rule_macro);

  RUN(test_keyword_entry);
  RUN(test_effect_decl);
  RUN(test_ignore_set);

  RUN(test_vpa_gen_input_empty);
  RUN(test_vpa_gen_input_with_rules);

  RUN(test_scope_handling);

  RUN(test_re_ir_for_vpa);
  RUN(test_re_ir_build_literal);

  RUN(test_chunk_scope_ids);
  RUN(test_token_scope_reference);

  RUN(test_vpa_unit_binary_mode);

  RUN(test_vpa_gen_empty_rules);
  RUN(test_vpa_gen_single_scope);
  RUN(test_vpa_gen_exec);
  RUN(test_vpa_gen_multi_scope);

  printf("all ok\n");
  return 0;
}
