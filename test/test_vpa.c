#include "../src/darray.h"
#include "../src/header_writer.h"
#include "../src/irwriter.h"
#include "../src/parse.h"
#include "../src/symtab.h"
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

static VpaActionUnits _make_au_tok(int32_t tok_id) {
  VpaActionUnits au = darray_new(sizeof(int32_t), 0);
  darray_push(au, tok_id);
  return au;
}

static void _free_gen_input(VpaGenInput* input) {
  for (int32_t i = 0; i < (int32_t)darray_size(input->scopes); i++) {
    VpaScope* s = &input->scopes[i];
    free(s->name);
    re_ir_free(s->leader.re);
    darray_del(s->leader.action_units);
    for (int32_t j = 0; j < (int32_t)darray_size(s->children); j++) {
      re_ir_free(s->children[j].re);
      darray_del(s->children[j].action_units);
    }
    darray_del(s->children);
  }
  darray_del(input->scopes);
  for (int32_t i = 0; i < (int32_t)darray_size(input->effect_decls); i++) {
    darray_del(input->effect_decls[i].effects);
  }
  darray_del(input->effect_decls);
  symtab_free(&input->tokens);
  symtab_free(&input->hooks);
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

static VpaGenInput _empty_input(void) {
  VpaGenInput input = {0};
  input.scopes = darray_new(sizeof(VpaScope), 0);
  input.effect_decls = darray_new(sizeof(EffectDecl), 0);
  symtab_init(&input.tokens, 1);
  symtab_init(&input.hooks, 0);
  symtab_intern(&input.hooks, ".begin");
  symtab_intern(&input.hooks, ".end");
  symtab_intern(&input.hooks, ".fail");
  symtab_intern(&input.hooks, ".unparse");
  return input;
}

// === vpa_gen: basic tests ===

TEST(test_vpa_gen_empty_rules) {
  VpaGenInput input = _empty_input();

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_gen_empty.h", BUILD_DIR "/test_vpa_gen_empty.ll");

  char* h_buf = _read_file(BUILD_DIR "/test_vpa_gen_empty.h");
  assert(strstr(h_buf, "#pragma once") != NULL);
  assert(strstr(h_buf, "vpa_lex") != NULL);
  free(h_buf);

  _compile_test(BUILD_DIR "/test_vpa_gen_empty.h", BUILD_DIR "/test_vpa_gen_empty.ll");
  _free_gen_input(&input);
}

TEST(test_vpa_gen_single_scope) {
  VpaGenInput input = _empty_input();

  int32_t tok_a_id = symtab_intern(&input.tokens, "tok_a");
  int32_t tok_b_id = symtab_intern(&input.tokens, "tok_b");

  VpaScope main_scope = {.scope_id = 0, .name = strdup("main"), .leader = {0}};
  main_scope.children = darray_new(sizeof(VpaUnit), 0);

  VpaUnit u1 = {.kind = VPA_RE, .re = re_ir_new(), .action_units = _make_au_tok(tok_a_id)};
  u1.re = re_ir_emit_ch(u1.re, 'a');
  darray_push(main_scope.children, u1);

  VpaUnit u2 = {.kind = VPA_RE, .re = re_ir_new(), .action_units = _make_au_tok(tok_b_id)};
  u2.re = re_ir_emit_ch(u2.re, 'b');
  darray_push(main_scope.children, u2);

  darray_push(input.scopes, main_scope);

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
  VpaGenInput input = _empty_input();

  int32_t tok_x_id = symtab_intern(&input.tokens, "tok_x");
  int32_t tok_y_id = symtab_intern(&input.tokens, "tok_y");

  // main scope
  VpaScope main_scope = {.scope_id = 0, .name = strdup("main"), .leader = {0}};
  main_scope.children = darray_new(sizeof(VpaUnit), 0);

  VpaUnit u_x = {.kind = VPA_RE, .re = re_ir_new(), .action_units = _make_au_tok(tok_x_id)};
  u_x.re = re_ir_emit_ch(u_x.re, 'x');
  darray_push(main_scope.children, u_x);

  darray_push(input.scopes, main_scope);

  // inner scope with leader regex
  VpaScope inner_scope = {.scope_id = 1, .name = strdup("inner")};
  inner_scope.leader = (VpaUnit){.kind = VPA_RE, .re = re_ir_new()};
  inner_scope.leader.re = re_ir_emit_ch(inner_scope.leader.re, '{');
  inner_scope.children = darray_new(sizeof(VpaUnit), 0);

  VpaUnit u_y = {.kind = VPA_RE, .re = re_ir_new(), .action_units = _make_au_tok(tok_y_id)};
  u_y.re = re_ir_emit_ch(u_y.re, 'y');
  darray_push(inner_scope.children, u_y);

  darray_push(input.scopes, inner_scope);

  // Add a call to inner scope in main's children
  VpaActionUnits call_au = darray_new(sizeof(int32_t), 0);
  int32_t begin_au = 0; // -HOOK_ID_BEGIN = 0
  darray_push(call_au, begin_au);
  VpaUnit call_u = {.kind = VPA_CALL, .call_scope_id = 1, .action_units = call_au};
  darray_push(input.scopes[0].children, call_u);

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

TEST(test_vpa_gen_user_hook) {
  VpaGenInput input = _empty_input();

  int32_t tok_x_id = symtab_intern(&input.tokens, "tok_x");
  int32_t hook_on_x = symtab_intern(&input.hooks, ".on_x");

  VpaScope main_scope = {.scope_id = 0, .name = strdup("main"), .leader = {0}};
  main_scope.children = darray_new(sizeof(VpaUnit), 0);

  VpaActionUnits au = darray_new(sizeof(int32_t), 0);
  darray_push(au, tok_x_id);
  int32_t hook_au = -hook_on_x;
  darray_push(au, hook_au);

  VpaUnit u = {.kind = VPA_RE, .re = re_ir_new(), .action_units = au};
  u.re = re_ir_emit_ch(u.re, 'x');
  darray_push(main_scope.children, u);

  darray_push(input.scopes, main_scope);

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_hook.h", BUILD_DIR "/test_vpa_hook.ll");

  char* h_buf = _read_file(BUILD_DIR "/test_vpa_hook.h");
  assert(strstr(h_buf, "on_x") != NULL);
  assert(strstr(h_buf, "ParseContext") != NULL);
  free(h_buf);

  char* ir_buf = _read_file(BUILD_DIR "/test_vpa_hook.ll");
  assert(strstr(ir_buf, "@vpa_hook_on_x") != NULL);
  assert(strstr(ir_buf, "call void @vpa_hook_on_x") != NULL);
  assert(strstr(ir_buf, "tc_add") != NULL);
  free(ir_buf);

  _compile_test(BUILD_DIR "/test_vpa_hook.h", BUILD_DIR "/test_vpa_hook.ll");
  _free_gen_input(&input);
}

// === vpa_gen: macro rules skipped in scope collection ===
// (No macro concept in VpaGenInput — macros are resolved by post_process before vpa_gen)

TEST(test_vpa_gen_token_dedup) {
  VpaGenInput input = _empty_input();

  // Two units with same token name -> same tok_id
  int32_t ws_id = symtab_intern(&input.tokens, "ws");
  int32_t other_id = symtab_intern(&input.tokens, "other");

  VpaScope main_scope = {.scope_id = 0, .name = strdup("main"), .leader = {0}};
  main_scope.children = darray_new(sizeof(VpaUnit), 0);

  VpaUnit u1 = {.kind = VPA_RE, .re = re_ir_new(), .action_units = _make_au_tok(ws_id)};
  u1.re = re_ir_emit_ch(u1.re, ' ');
  darray_push(main_scope.children, u1);

  VpaUnit u2 = {.kind = VPA_RE, .re = re_ir_new(), .action_units = _make_au_tok(ws_id)};
  u2.re = re_ir_emit_ch(u2.re, '\t');
  darray_push(main_scope.children, u2);

  VpaUnit u3 = {.kind = VPA_RE, .re = re_ir_new(), .action_units = _make_au_tok(other_id)};
  u3.re = re_ir_emit_ch(u3.re, 'x');
  darray_push(main_scope.children, u3);

  darray_push(input.scopes, main_scope);

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_dedup.h", BUILD_DIR "/test_vpa_dedup.ll");

  char* h_buf = _read_file(BUILD_DIR "/test_vpa_dedup.h");
  const char* first = strstr(h_buf, "TOK_WS");
  assert(first != NULL);
  const char* second = strstr(first + 6, "TOK_WS");
  assert(second == NULL);
  assert(strstr(h_buf, "TOK_OTHER") != NULL);
  // ws and other have same action_units so they dedup to 1 action each
  assert(strstr(h_buf, "VPA_N_ACTIONS 2") != NULL);
  free(h_buf);

  _compile_test(BUILD_DIR "/test_vpa_dedup.h", BUILD_DIR "/test_vpa_dedup.ll");
  _free_gen_input(&input);
}

// === vpa_gen: empty scope body -> stub DFA ===

TEST(test_vpa_gen_empty_scope_body) {
  VpaGenInput input = _empty_input();

  int32_t tok_a_id = symtab_intern(&input.tokens, "tok_a");

  VpaScope main_scope = {.scope_id = 0, .name = strdup("main"), .leader = {0}};
  main_scope.children = darray_new(sizeof(VpaUnit), 0);

  VpaUnit u1 = {.kind = VPA_RE, .re = re_ir_new(), .action_units = _make_au_tok(tok_a_id)};
  u1.re = re_ir_emit_ch(u1.re, 'a');
  darray_push(main_scope.children, u1);

  darray_push(input.scopes, main_scope);

  // empty scope
  VpaScope empty_scope = {.scope_id = 1, .name = strdup("empty_scope")};
  empty_scope.leader = (VpaUnit){.kind = VPA_RE, .re = re_ir_new()};
  empty_scope.leader.re = re_ir_emit_ch(empty_scope.leader.re, '(');
  empty_scope.children = darray_new(sizeof(VpaUnit), 0);
  darray_push(input.scopes, empty_scope);

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_empty_body.h", BUILD_DIR "/test_vpa_empty_body.ll");

  char* ir_buf = _read_file(BUILD_DIR "/test_vpa_empty_body.ll");
  assert(strstr(ir_buf, "lex_empty_scope") != NULL);
  assert(strstr(ir_buf, "lex_main") != NULL);
  free(ir_buf);

  _compile_test(BUILD_DIR "/test_vpa_empty_body.h", BUILD_DIR "/test_vpa_empty_body.ll");
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
  RUN(test_vpa_gen_token_dedup);
  RUN(test_vpa_gen_empty_scope_body);

  printf("all ok\n");
  return 0;
}
