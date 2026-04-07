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

  TokenChunk* child = tc_push(tree, 0);
  assert(child != NULL);
  assert(tree->current == child);
  assert(child->parent_id != -1);

  tc_add(tree, TOK_VPA_ID, 0, 2, -1);
  assert(darray_size(child->tokens) == 1);

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

  TokenChunk* l1 = tc_push(tree, 0);
  assert(tree->current == l1);

  TokenChunk* l2 = tc_push(tree, 0);
  assert(tree->current == l2);

  tc_pop(tree);
  assert(tree->current == l1);

  tc_pop(tree);
  assert(tree->current == root);

  tc_tree_del(tree);
  ustr_del(ustr);
}

TEST(test_tree_locate_single_line) {
  char* ustr = ustr_new(5, "hello");
  TokenTree* tree = tc_tree_new(ustr);

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
  char* ustr = ustr_new(7, "ab\ncd\ne");
  TokenTree* tree = tc_tree_new(ustr);

  tree->newline_map[0] |= (1ULL << 2);
  tree->newline_map[0] |= (1ULL << 5);

  Location loc;
  loc = tc_locate(tree, 0);
  assert(loc.line == 0 && loc.col == 0);

  loc = tc_locate(tree, 1);
  assert(loc.line == 0 && loc.col == 1);

  loc = tc_locate(tree, 3);
  assert(loc.line == 1 && loc.col == 0);

  loc = tc_locate(tree, 4);
  assert(loc.line == 1 && loc.col == 1);

  loc = tc_locate(tree, 6);
  assert(loc.line == 2 && loc.col == 0);

  tc_tree_del(tree);
  ustr_del(ustr);
}

TEST(test_token_size) { assert(sizeof(Token) == 16); }

// === ReIr for VPA units ===

TEST(test_re_ir_for_vpa) {
  ReIr ir = re_ir_new();
  assert(darray_size(ir) == 0);

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

TEST(test_token_scope_reference) {
  char* ustr = ustr_new(3, "abc");
  TokenTree* tree = tc_tree_new(ustr);

  tc_push(tree, SCOPE_VPA);
  int32_t child_chunk_id = (int32_t)(darray_size(tree->table) - 1);

  tc_pop(tree);

  tc_add(tree, SCOPE_VPA, 0, 3, child_chunk_id);

  assert(darray_size(tree->root->tokens) == 1);
  Token stored = tree->root->tokens[0];
  assert(stored.tok_id == SCOPE_VPA);
  assert(stored.tok_id < SCOPE_COUNT);
  assert(stored.chunk_id == child_chunk_id);

  assert(tree->table[stored.chunk_id].scope_id == SCOPE_VPA);

  tc_tree_del(tree);
  ustr_del(ustr);
}

// === vpa_gen helpers ===

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
  for (int32_t i = 0; i < (int32_t)darray_size(input->effects); i++) {
    free(input->effects[i].hook_name);
    darray_del(input->effects[i].effects);
  }
  darray_del(input->effects);
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

static char* _read_file(const char* path) {
  FILE* f = fopen(path, "r");
  assert(f);
  fseek(f, 0, SEEK_END);
  size_t sz = (size_t)ftell(f);
  rewind(f);
  char* buf = malloc(sz + 1);
  fread(buf, 1, sz, f);
  buf[sz] = '\0';
  fclose(f);
  return buf;
}

static void _run_vpa_gen(VpaGenInput* input, const char* h_path, const char* ir_path) {
  FILE* hf = fopen(h_path, "w");
  FILE* irf = fopen(ir_path, "w");
  assert(hf && irf);
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test_vpa.c", ".");
  vpa_gen(input, hw, w, NULL);
  irwriter_end(w);

  hw_del(hw);
  irwriter_del(w);
  fclose(irf);
  fclose(hf);
}

static VpaGenInput _empty_input(const char* src) {
  VpaGenInput input = {0};
  input.rules = darray_new(sizeof(VpaRule), 0);
  input.effects = darray_new(sizeof(EffectDecl), 0);
  input.src = src;
  return input;
}

// === vpa_gen: basic tests ===

TEST(test_vpa_gen_empty_rules) {
  VpaGenInput input = _empty_input("");

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_gen_empty.h", BUILD_DIR "/test_vpa_gen_empty.ll");

  char* h_buf = _read_file(BUILD_DIR "/test_vpa_gen_empty.h");
  assert(strstr(h_buf, "#pragma once") != NULL);
  assert(strstr(h_buf, "vpa_lex") != NULL);
  free(h_buf);

  _compile_test(BUILD_DIR "/test_vpa_gen_empty.h", BUILD_DIR "/test_vpa_gen_empty.ll");
  _free_gen_input(&input);
}

TEST(test_vpa_gen_single_scope) {
  VpaGenInput input = _empty_input("");

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

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_gen_scope.h", BUILD_DIR "/test_vpa_gen_scope.ll");

  char* ir_buf = _read_file(BUILD_DIR "/test_vpa_gen_scope.ll");
  assert(strstr(ir_buf, "define {i64, i64} @lex_main") != NULL);
  assert(strstr(ir_buf, "@vpa_dispatch") != NULL);
  assert(strstr(ir_buf, "@vpa_lex") != NULL);
  free(ir_buf);

  char* h_buf = _read_file(BUILD_DIR "/test_vpa_gen_scope.h");
  assert(strstr(h_buf, "SCOPE_MAIN") != NULL);
  assert(strstr(h_buf, "TOK_TOK_A") != NULL);
  assert(strstr(h_buf, "TOK_TOK_B") != NULL);
  assert(strstr(h_buf, "tc_tree_new") != NULL);
  free(h_buf);

  _compile_test(BUILD_DIR "/test_vpa_gen_scope.h", BUILD_DIR "/test_vpa_gen_scope.ll");
  _free_gen_input(&input);
}

TEST(test_vpa_gen_multi_scope) {
  VpaGenInput input = _empty_input("");

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

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_gen_multi.h", BUILD_DIR "/test_vpa_gen_multi.ll");

  char* ir_buf = _read_file(BUILD_DIR "/test_vpa_gen_multi.ll");
  assert(strstr(ir_buf, "lex_main") != NULL);
  assert(strstr(ir_buf, "lex_inner") != NULL);
  free(ir_buf);

  char* h_buf = _read_file(BUILD_DIR "/test_vpa_gen_multi.h");
  assert(strstr(h_buf, "SCOPE_MAIN") != NULL);
  assert(strstr(h_buf, "SCOPE_INNER") != NULL);
  free(h_buf);

  _compile_test(BUILD_DIR "/test_vpa_gen_multi.h", BUILD_DIR "/test_vpa_gen_multi.ll");
  _free_gen_input(&input);
}

TEST(test_vpa_gen_exec) {
  const char* driver_path = BUILD_DIR "/test_vpa_exec_driver.c";
  FILE* df = fopen(driver_path, "w");
  assert(df);
  fprintf(df, "#include <assert.h>\n"
              "#include <stdint.h>\n"
              "#include <string.h>\n"
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

// === vpa_gen: user hooks ===
// A unit with .user_hook should produce:
//   header: "void vpa_hook_<name>(...)"
//   IR: declare + call to @vpa_hook_<name>

TEST(test_vpa_gen_user_hook) {
  VpaGenInput input = _empty_input("");

  VpaRule main_rule = {0};
  main_rule.name = strdup("main");
  main_rule.units = darray_new(sizeof(VpaUnit), 0);
  main_rule.is_scope = true;

  VpaUnit u = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("tok_x")};
  u.re = re_ir_emit_ch(u.re, 'x');
  u.user_hook = strdup(".on_x");
  darray_push(main_rule.units, u);

  darray_push(input.rules, main_rule);

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_hook.h", BUILD_DIR "/test_vpa_hook.ll");

  char* h_buf = _read_file(BUILD_DIR "/test_vpa_hook.h");
  assert(strstr(h_buf, "vpa_hook_on_x") != NULL);
  assert(strstr(h_buf, "void vpa_hook_on_x") != NULL);
  free(h_buf);

  char* ir_buf = _read_file(BUILD_DIR "/test_vpa_hook.ll");
  assert(strstr(ir_buf, "@vpa_hook_on_x") != NULL);
  assert(strstr(ir_buf, "call void @vpa_hook_on_x") != NULL);
  assert(strstr(ir_buf, "tc_add") != NULL);
  free(ir_buf);

  _compile_test(BUILD_DIR "/test_vpa_hook.h", BUILD_DIR "/test_vpa_hook.ll");
  _free_gen_input(&input);
}

// === vpa_gen: pop_scope via TOK_HOOK_END ===

TEST(test_vpa_gen_pop_scope) {
  VpaGenInput input = _empty_input("");

  VpaRule main_rule = {0};
  main_rule.name = strdup("main");
  main_rule.units = darray_new(sizeof(VpaUnit), 0);
  main_rule.is_scope = true;

  VpaUnit u1 = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("tok_a")};
  u1.re = re_ir_emit_ch(u1.re, 'a');
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

  VpaUnit closer = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("close")};
  closer.re = re_ir_emit_ch(closer.re, '}');
  closer.hook = TOK_HOOK_END;
  darray_push(scope_body.children, closer);

  darray_push(inner_rule.units, scope_body);
  darray_push(input.rules, inner_rule);

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_pop.h", BUILD_DIR "/test_vpa_pop.ll");

  char* ir_buf = _read_file(BUILD_DIR "/test_vpa_pop.ll");
  assert(strstr(ir_buf, "tc_pop") != NULL);
  assert(strstr(ir_buf, "tc_add") != NULL);
  assert(strstr(ir_buf, "lex_main") != NULL);
  assert(strstr(ir_buf, "lex_inner") != NULL);
  free(ir_buf);

  _compile_test(BUILD_DIR "/test_vpa_pop.h", BUILD_DIR "/test_vpa_pop.ll");
  _free_gen_input(&input);
}

// === vpa_gen: literal tokens ===
// Literal tokens named lit.xxx should produce TOK_LIT_XXX defines in the header.

TEST(test_vpa_gen_keywords) {
  VpaGenInput input = _empty_input("");

  VpaRule main_rule = {0};
  main_rule.name = strdup("main");
  main_rule.units = darray_new(sizeof(VpaUnit), 0);
  main_rule.is_scope = true;

  VpaUnit u = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("ident")};
  u.re = re_ir_emit_ch(u.re, 'x');
  darray_push(main_rule.units, u);

  VpaUnit kw1 = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("lit.int")};
  kw1.re = re_ir_emit_ch(kw1.re, 'i');
  kw1.re = re_ir_emit_ch(kw1.re, 'n');
  kw1.re = re_ir_emit_ch(kw1.re, 't');
  darray_push(main_rule.units, kw1);

  VpaUnit kw2 = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("lit.float")};
  kw2.re = re_ir_emit_ch(kw2.re, 'f');
  kw2.re = re_ir_emit_ch(kw2.re, 'l');
  kw2.re = re_ir_emit_ch(kw2.re, 'o');
  kw2.re = re_ir_emit_ch(kw2.re, 'a');
  kw2.re = re_ir_emit_ch(kw2.re, 't');
  darray_push(main_rule.units, kw2);

  darray_push(input.rules, main_rule);

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_kw.h", BUILD_DIR "/test_vpa_kw.ll");

  char* h_buf = _read_file(BUILD_DIR "/test_vpa_kw.h");
  assert(strstr(h_buf, "TOK_LIT_INT") != NULL);
  assert(strstr(h_buf, "TOK_LIT_FLOAT") != NULL);
  assert(strstr(h_buf, "TOK_IDENT") != NULL);
  free(h_buf);

  _compile_test(BUILD_DIR "/test_vpa_kw.h", BUILD_DIR "/test_vpa_kw.ll");
  _free_gen_input(&input);
}

// === vpa_gen: effects causing pop_scope ===
// An effect declaration mapping a user hook to TOK_HOOK_END should trigger tc_pop.

TEST(test_vpa_gen_effect_pop) {
  VpaGenInput input = _empty_input("");

  VpaRule main_rule = {0};
  main_rule.name = strdup("main");
  main_rule.units = darray_new(sizeof(VpaUnit), 0);
  main_rule.is_scope = true;

  VpaUnit u1 = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("tok_a")};
  u1.re = re_ir_emit_ch(u1.re, 'a');
  darray_push(main_rule.units, u1);

  VpaUnit ref = {.kind = VPA_REF, .name = strdup("braced")};
  darray_push(main_rule.units, ref);
  darray_push(input.rules, main_rule);

  VpaRule braced = {0};
  braced.name = strdup("braced");
  braced.units = darray_new(sizeof(VpaUnit), 0);
  braced.is_scope = false;

  VpaUnit bleader = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("open")};
  bleader.re = re_ir_emit_ch(bleader.re, '{');
  darray_push(braced.units, bleader);

  VpaUnit scope_body = {.kind = VPA_SCOPE, .name = strdup("braced_body"), .hook = TOK_HOOK_BEGIN};
  scope_body.re = re_ir_new();
  scope_body.re = re_ir_emit_ch(scope_body.re, '{');
  scope_body.children = darray_new(sizeof(VpaUnit), 0);

  VpaUnit inner = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("tok_y")};
  inner.re = re_ir_emit_ch(inner.re, 'y');
  darray_push(scope_body.children, inner);

  // closer has user_hook; effect decl maps it to TOK_HOOK_END
  VpaUnit closer = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("close")};
  closer.re = re_ir_emit_ch(closer.re, '}');
  closer.user_hook = strdup(".close_brace");
  darray_push(scope_body.children, closer);

  darray_push(braced.units, scope_body);
  darray_push(input.rules, braced);

  EffectDecl ed = {0};
  ed.hook_name = strdup(".close_brace");
  ed.effects = darray_new(sizeof(int32_t), 0);
  int32_t end_effect = TOK_HOOK_END;
  darray_push(ed.effects, end_effect);
  darray_push(input.effects, ed);

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_effect.h", BUILD_DIR "/test_vpa_effect.ll");

  char* ir_buf = _read_file(BUILD_DIR "/test_vpa_effect.ll");
  assert(strstr(ir_buf, "tc_pop") != NULL);
  assert(strstr(ir_buf, "vpa_hook_close_brace") != NULL);
  free(ir_buf);

  char* h_buf = _read_file(BUILD_DIR "/test_vpa_effect.h");
  assert(strstr(h_buf, "vpa_hook_close_brace") != NULL);
  free(h_buf);

  _compile_test(BUILD_DIR "/test_vpa_effect.h", BUILD_DIR "/test_vpa_effect.ll");
  _free_gen_input(&input);
}

// === vpa_gen: macro rules skipped in scope collection ===

TEST(test_vpa_gen_macro_skip) {
  VpaGenInput input = _empty_input("");

  VpaRule main_rule = {0};
  main_rule.name = strdup("main");
  main_rule.units = darray_new(sizeof(VpaUnit), 0);
  main_rule.is_scope = true;

  VpaUnit u = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("tok_a")};
  u.re = re_ir_emit_ch(u.re, 'a');
  darray_push(main_rule.units, u);
  darray_push(input.rules, main_rule);

  // macro: should NOT become a scope or lex_ function
  VpaRule macro = {0};
  macro.name = strdup("_helper");
  macro.units = darray_new(sizeof(VpaUnit), 0);
  macro.is_scope = false;
  macro.is_macro = true;

  VpaUnit mu = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("tok_m")};
  mu.re = re_ir_emit_ch(mu.re, 'm');
  darray_push(macro.units, mu);
  darray_push(input.rules, macro);

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_macro.h", BUILD_DIR "/test_vpa_macro.ll");

  char* ir_buf = _read_file(BUILD_DIR "/test_vpa_macro.ll");
  assert(strstr(ir_buf, "lex_main") != NULL);
  assert(strstr(ir_buf, "lex__helper") == NULL);
  free(ir_buf);

  char* h_buf = _read_file(BUILD_DIR "/test_vpa_macro.h");
  assert(strstr(h_buf, "SCOPE_MAIN") != NULL);
  assert(strstr(h_buf, "SCOPE__HELPER") == NULL);
  assert(strstr(h_buf, "VPA_N_SCOPES 1") != NULL);
  free(h_buf);

  _compile_test(BUILD_DIR "/test_vpa_macro.h", BUILD_DIR "/test_vpa_macro.ll");
  _free_gen_input(&input);
}

// === vpa_gen: token dedup ===
// Two units with same name → same token ID, only one TOK_ define.

TEST(test_vpa_gen_token_dedup) {
  VpaGenInput input = _empty_input("");

  VpaRule main_rule = {0};
  main_rule.name = strdup("main");
  main_rule.units = darray_new(sizeof(VpaUnit), 0);
  main_rule.is_scope = true;

  VpaUnit u1 = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("ws")};
  u1.re = re_ir_emit_ch(u1.re, ' ');
  darray_push(main_rule.units, u1);

  VpaUnit u2 = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("ws")};
  u2.re = re_ir_emit_ch(u2.re, '\t');
  darray_push(main_rule.units, u2);

  VpaUnit u3 = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("other")};
  u3.re = re_ir_emit_ch(u3.re, 'x');
  darray_push(main_rule.units, u3);

  darray_push(input.rules, main_rule);

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_dedup.h", BUILD_DIR "/test_vpa_dedup.ll");

  char* h_buf = _read_file(BUILD_DIR "/test_vpa_dedup.h");
  const char* first = strstr(h_buf, "TOK_WS");
  assert(first != NULL);
  const char* second = strstr(first + 6, "TOK_WS");
  assert(second == NULL);
  assert(strstr(h_buf, "TOK_OTHER") != NULL);
  assert(strstr(h_buf, "VPA_N_ACTIONS 2") != NULL);
  free(h_buf);

  _compile_test(BUILD_DIR "/test_vpa_dedup.h", BUILD_DIR "/test_vpa_dedup.ll");
  _free_gen_input(&input);
}

// === vpa_gen: empty scope body → stub DFA ===

TEST(test_vpa_gen_empty_scope_body) {
  VpaGenInput input = _empty_input("");

  VpaRule main_rule = {0};
  main_rule.name = strdup("main");
  main_rule.units = darray_new(sizeof(VpaUnit), 0);
  main_rule.is_scope = true;

  VpaUnit u = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("tok_a")};
  u.re = re_ir_emit_ch(u.re, 'a');
  darray_push(main_rule.units, u);

  VpaUnit ref = {.kind = VPA_REF, .name = strdup("empty_scope")};
  darray_push(main_rule.units, ref);
  darray_push(input.rules, main_rule);

  VpaRule empty = {0};
  empty.name = strdup("empty_scope");
  empty.units = darray_new(sizeof(VpaUnit), 0);
  empty.is_scope = false;

  VpaUnit leader = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("open")};
  leader.re = re_ir_emit_ch(leader.re, '(');
  darray_push(empty.units, leader);

  VpaUnit scope_body = {.kind = VPA_SCOPE, .name = strdup("empty_scope_body"), .hook = TOK_HOOK_BEGIN};
  scope_body.re = re_ir_new();
  scope_body.re = re_ir_emit_ch(scope_body.re, '(');
  scope_body.children = darray_new(sizeof(VpaUnit), 0);

  darray_push(empty.units, scope_body);
  darray_push(input.rules, empty);

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_empty_body.h", BUILD_DIR "/test_vpa_empty_body.ll");

  char* ir_buf = _read_file(BUILD_DIR "/test_vpa_empty_body.ll");
  assert(strstr(ir_buf, "lex_empty_scope") != NULL);
  assert(strstr(ir_buf, "lex_main") != NULL);
  free(ir_buf);

  _compile_test(BUILD_DIR "/test_vpa_empty_body.h", BUILD_DIR "/test_vpa_empty_body.ll");
  _free_gen_input(&input);
}

// === vpa_gen: pop_scope execution test ===
// Compile + link + run a lexer with scope push/pop.

TEST(test_vpa_gen_pop_exec) {
  // first generate the IR (reuse pop_scope setup)
  VpaGenInput input = _empty_input("");

  VpaRule main_rule = {0};
  main_rule.name = strdup("main");
  main_rule.units = darray_new(sizeof(VpaUnit), 0);
  main_rule.is_scope = true;

  VpaUnit u1 = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("tok_a")};
  u1.re = re_ir_emit_ch(u1.re, 'a');
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

  VpaUnit inner_tok = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("tok_b")};
  inner_tok.re = re_ir_emit_ch(inner_tok.re, 'b');
  darray_push(scope_body.children, inner_tok);

  VpaUnit closer = {.kind = VPA_REGEXP, .re = re_ir_new(), .name = strdup("close")};
  closer.re = re_ir_emit_ch(closer.re, '}');
  closer.hook = TOK_HOOK_END;
  darray_push(scope_body.children, closer);

  darray_push(inner_rule.units, scope_body);
  darray_push(input.rules, inner_rule);

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_pop_exec.h", BUILD_DIR "/test_vpa_pop_exec.ll");

  // write driver
  const char* driver_path = BUILD_DIR "/test_vpa_pop_exec_driver.c";
  FILE* df = fopen(driver_path, "w");
  assert(df);
  fprintf(df, "#include <assert.h>\n"
              "#include <stdint.h>\n"
              "#include \"test_vpa_pop_exec.h\"\n"
              "\n"
              "int32_t vpa_rt_read_cp(void* src, int32_t cp_off) {\n"
              "  return ((const unsigned char*)src)[cp_off];\n"
              "}\n"
              "\n"
              "int main(void) {\n"
              "  // input: \"a{bb}a\" -> main sees 'a', '{' pushes inner, inner sees bb},\n"
              "  //   '}' pops back to main, main sees final 'a'\n"
              "  const char* input = \"a{bb}a\";\n"
              "  int32_t len = 6;\n"
              "  char* us = ustr_new(len, input);\n"
              "  TokenTree* tt = tc_tree_new(us);\n"
              "  vpa_lex((int64_t)(intptr_t)input, (int64_t)len, (int64_t)(intptr_t)tt);\n"
              "  // root: tok_a at 0, tok_a at 5  ('{' triggers scope push, no token)\n"
              "  int32_t n = (int32_t)darray_size(tt->root->tokens);\n"
              "  assert(n == 2);\n"
              "  assert(tt->root->tokens[0].tok_id == TOK_TOK_A);\n"
              "  assert(tt->root->tokens[0].cp_start == 0);\n"
              "  assert(tt->root->tokens[1].tok_id == TOK_TOK_A);\n"
              "  assert(tt->root->tokens[1].cp_start == 5);\n"
              "  // inner scope chunk should have: tok_b, tok_b, close\n"
              "  int32_t tsize = (int32_t)darray_size(tt->table);\n"
              "  assert(tsize == 2);  // root + inner\n"
              "  TokenChunk* inner = &tt->table[1];\n"
              "  assert(inner->scope_id == SCOPE_INNER);\n"
              "  int32_t in = (int32_t)darray_size(inner->tokens);\n"
              "  assert(in == 3);\n"
              "  assert(inner->tokens[0].tok_id == TOK_TOK_B);\n"
              "  assert(inner->tokens[0].cp_start == 2);\n"
              "  assert(inner->tokens[1].tok_id == TOK_TOK_B);\n"
              "  assert(inner->tokens[1].cp_start == 3);\n"
              "  assert(inner->tokens[2].tok_id == TOK_CLOSE);\n"
              "  assert(inner->tokens[2].cp_start == 4);\n"
              "  tc_tree_del(tt);\n"
              "  ustr_del(us);\n"
              "  return 0;\n"
              "}\n");
  fclose(df);

  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "%s %s %s -o %s 2>&1", compat_llvm_cc(), driver_path, BUILD_DIR "/test_vpa_pop_exec.ll",
           BUILD_DIR "/test_vpa_pop_exec_bin");
  int ret = system(cmd);
  assert(ret == 0);

  snprintf(cmd, sizeof(cmd), "%s", BUILD_DIR "/test_vpa_pop_exec_bin");
  ret = system(cmd);
  assert(ret == 0);

  _free_gen_input(&input);
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

  RUN(test_re_ir_for_vpa);
  RUN(test_re_ir_build_literal);

  RUN(test_chunk_scope_ids);
  RUN(test_token_scope_reference);

  RUN(test_vpa_gen_empty_rules);
  RUN(test_vpa_gen_single_scope);
  RUN(test_vpa_gen_exec);
  RUN(test_vpa_gen_multi_scope);

  RUN(test_vpa_gen_user_hook);
  RUN(test_vpa_gen_pop_scope);
  RUN(test_vpa_gen_keywords);
  RUN(test_vpa_gen_effect_pop);
  RUN(test_vpa_gen_macro_skip);
  RUN(test_vpa_gen_token_dedup);
  RUN(test_vpa_gen_empty_scope_body);
  RUN(test_vpa_gen_pop_exec);

  printf("all ok\n");
  return 0;
}
