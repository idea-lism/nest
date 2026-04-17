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
  TokenTree* tree = tt_tree_new(ustr);
  assert(tree != NULL);
  assert(tree->src == ustr);
  assert(tree->root != NULL);
  assert(tree->current == tree->root);
  tt_tree_del(tree, false);
  ustr_del(ustr);
}

TEST(test_tree_add_token) {
  char* ustr = ustr_new(3, "abc");
  TokenTree* tree = tt_tree_new(ustr);

  tt_add(tree, TOK_CHAR, 0, 1, -1);
  tt_add(tree, TOK_CHAR, 1, 1, -1);

  assert(darray_size(tree->current->tokens) == 2);
  assert(tree->current->tokens[0].term_id == TOK_CHAR);
  assert(tree->current->tokens[0].cp_start == 0);
  assert(tree->current->tokens[1].cp_start == 1);

  tt_tree_del(tree, false);
  ustr_del(ustr);
}

TEST(test_tree_push_pop) {
  char* ustr = ustr_new(5, "ab\ncd");
  TokenTree* tree = tt_tree_new(ustr);

  TokenChunk* root = tree->current;
  assert(root->parent_id == -1);

  TokenChunk* child = tt_push(tree, 0);
  assert(child != NULL);
  assert(tree->current == child);
  assert(child->parent_id != -1);

  tt_add(tree, TOK_VPA_ID, 0, 2, -1);
  assert(darray_size(child->tokens) == 1);

  TokenChunk* popped = tt_pop(tree, 0);
  assert(popped != NULL);
  assert(tree->current == root);

  tt_tree_del(tree, false);
  ustr_del(ustr);
}

TEST(test_tree_nested_push_pop) {
  char* ustr = ustr_new(1, "x");
  TokenTree* tree = tt_tree_new(ustr);

  TokenChunk* root = tree->current;

  TokenChunk* l1 = tt_push(tree, 0);
  assert(tree->current == l1);

  TokenChunk* l2 = tt_push(tree, 0);
  assert(tree->current == l2);

  tt_pop(tree, 0);
  assert(tree->current == l1);

  tt_pop(tree, 0);
  assert(tree->current == root);

  tt_tree_del(tree, false);
  ustr_del(ustr);
}

TEST(test_tree_locate_single_line) {
  char* ustr = ustr_new(5, "hello");
  TokenTree* tree = tt_tree_new(ustr);

  Location loc = tt_locate(tree, 0);
  assert(loc.line == 1);
  assert(loc.col == 1);

  loc = tt_locate(tree, 3);
  assert(loc.line == 1);
  assert(loc.col == 4);

  tt_tree_del(tree, false);
  ustr_del(ustr);
}

TEST(test_tree_locate_multiline) {
  char* ustr = ustr_new(7, "ab\ncd\ne");
  TokenTree* tree = tt_tree_new(ustr);

  // manually set newline bits (lexer would do this during codepoint feeding)
  tree->newline_map[0] |= (1ULL << 2);
  tree->newline_map[0] |= (1ULL << 5);

  Location loc;
  loc = tt_locate(tree, 0);
  assert(loc.line == 1 && loc.col == 1);

  loc = tt_locate(tree, 1);
  assert(loc.line == 1 && loc.col == 2);

  loc = tt_locate(tree, 3);
  assert(loc.line == 2 && loc.col == 1);

  loc = tt_locate(tree, 4);
  assert(loc.line == 2 && loc.col == 2);

  loc = tt_locate(tree, 6);
  assert(loc.line == 3 && loc.col == 1);

  tt_tree_del(tree, false);
  ustr_del(ustr);
}

TEST(test_token_size) { assert(sizeof(Token) == 16); }

// === ReIr for VPA units ===

TEST(test_re_ir_for_vpa) {
  ReIr ir = re_ir_new();
  assert(darray_size(ir) == 0);

  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0, 0, 0);
  ir = re_ir_emit_ch(ir, 'a');
  ir = re_ir_emit(ir, RE_IR_FORK, 0, 0, 0, 0);
  ir = re_ir_emit_ch(ir, 'b');
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0, 0, 0);

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
  TokenTree* tree = tt_tree_new(ustr);

  tree->root->scope_id = SCOPE_MAIN;

  tt_push(tree, SCOPE_VPA);
  assert(tree->current->scope_id == SCOPE_VPA);

  tt_push(tree, SCOPE_RE);
  assert(tree->current->scope_id == SCOPE_RE);

  tt_pop(tree, 0);
  assert(tree->current->scope_id == SCOPE_VPA);

  tt_pop(tree, 0);
  assert(tree->current->scope_id == SCOPE_MAIN);

  tt_tree_del(tree, false);
  ustr_del(ustr);
}

TEST(test_token_scope_reference) {
  char* ustr = ustr_new(3, "abc");
  TokenTree* tree = tt_tree_new(ustr);

  tt_push(tree, SCOPE_VPA);
  tt_add(tree, 1, 0, 1, -1); // add a token so child has content
  tt_pop(tree, 3);           // cp_end=3 covers "abc"

  // tt_pop added scope-ref token to root
  assert(darray_size(tree->root->tokens) == 1);
  Token stored = tree->root->tokens[0];
  assert(stored.term_id == SCOPE_VPA);
  assert(stored.term_id < SCOPE_COUNT);
  assert(stored.chunk_id == 1); // child is table[1]
  assert(stored.cp_start == 0);
  assert(stored.cp_size == 3);
  assert(tree->table[stored.chunk_id].scope_id == SCOPE_VPA);

  tt_tree_del(tree, false);
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
  HeaderWriter* hw = hdwriter_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test_vpa.c", ".");
  vpa_gen(input, hw, w, NULL, 0);
  irwriter_end(w);

  hdwriter_del(hw);
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

  int32_t tok_a_id = symtab_intern(&input.tokens, "@tok_a");
  int32_t tok_b_id = symtab_intern(&input.tokens, "@tok_b");

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
  assert(strstr(h_buf, "tt_tree_new") != NULL);
  free(h_buf);

  _compile_test(BUILD_DIR "/test_vpa_gen_scope.h", BUILD_DIR "/test_vpa_gen_scope.ll");
  _free_gen_input(&input);
}

TEST(test_vpa_gen_multi_scope) {
  VpaGenInput input = _empty_input();

  int32_t tok_x_id = symtab_intern(&input.tokens, "@tok_x");
  int32_t tok_y_id = symtab_intern(&input.tokens, "@tok_y");

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
              "int32_t tt_depth(void* tt) { return 1; }\n"
              "\n"
              "int main(void) {\n"
              "  const char* input = \"aabb\";\n"
              "  int32_t len = 4;\n"
              "  char* us = ustr_new(len, input);\n"
              "  TokenTree* tt = tt_tree_new(us);\n"
              "  vpa_lex((void*)input, len, (void*)tt, (void*)0, (void*)0);\n"
              "  int32_t n = (int32_t)darray_size(tt->root->tokens);\n"
              "  assert(n == 4);\n"
              "  assert(tt->root->tokens[0].term_id == TOK_TOK_A);\n"
              "  assert(tt->root->tokens[1].term_id == TOK_TOK_A);\n"
              "  assert(tt->root->tokens[2].term_id == TOK_TOK_B);\n"
              "  assert(tt->root->tokens[3].term_id == TOK_TOK_B);\n"
              "  assert(tt->root->tokens[0].cp_start == 0);\n"
              "  assert(tt->root->tokens[0].cp_size == 1);\n"
              "  assert(tt->root->tokens[3].cp_start == 3);\n"
              "  tt_tree_del(tt, false);\n"
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

  int32_t tok_x_id = symtab_intern(&input.tokens, "@tok_x");
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
  assert(strstr(ir_buf, "call i32 @vpa_hook_on_x") != NULL);
  assert(strstr(ir_buf, "tt_add") != NULL);
  free(ir_buf);

  _compile_test(BUILD_DIR "/test_vpa_hook.h", BUILD_DIR "/test_vpa_hook.ll");
  _free_gen_input(&input);
}

// === vpa_gen: macro rules skipped in scope collection ===
// (No macro concept in VpaGenInput — macros are resolved by post_process before vpa_gen)

TEST(test_vpa_gen_token_dedup) {
  VpaGenInput input = _empty_input();

  // Two units with same token name -> same tok_id
  int32_t ws_id = symtab_intern(&input.tokens, "@ws");
  int32_t other_id = symtab_intern(&input.tokens, "@other");

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
  free(h_buf);

  _compile_test(BUILD_DIR "/test_vpa_dedup.h", BUILD_DIR "/test_vpa_dedup.ll");
  _free_gen_input(&input);
}

TEST(test_vpa_gen_empty_scope_body) {
  VpaGenInput input = _empty_input();

  int32_t tok_a_id = symtab_intern(&input.tokens, "@tok_a");

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

TEST(test_vpa_gen_long_names) {
  VpaGenInput input = _empty_input();

  // 200-char token name — exceeds the old 128-char buffer
  char long_name[202];
  long_name[0] = '@';
  long_name[1] = 't';
  memset(long_name + 2, 'a', 199);
  long_name[201] = '\0';

  int32_t tok_id = symtab_intern(&input.tokens, long_name);

  VpaScope main_scope = {.scope_id = 0, .name = strdup("main"), .leader = {0}};
  main_scope.children = darray_new(sizeof(VpaUnit), 0);

  VpaUnit u = {.kind = VPA_RE, .re = re_ir_new(), .action_units = _make_au_tok(tok_id)};
  u.re = re_ir_emit_ch(u.re, 'z');
  darray_push(main_scope.children, u);

  darray_push(input.scopes, main_scope);

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_long.h", BUILD_DIR "/test_vpa_long.ll");

  // build expected uppercased define: "TOK_T" + 199 'A's
  char expected[210];
  memcpy(expected, "TOK_T", 5);
  memset(expected + 5, 'A', 199);
  expected[204] = '\0';

  char* h_buf = _read_file(BUILD_DIR "/test_vpa_long.h");
  assert(strstr(h_buf, expected) != NULL);
  free(h_buf);

  _compile_test(BUILD_DIR "/test_vpa_long.h", BUILD_DIR "/test_vpa_long.ll");
  _free_gen_input(&input);
}

static void _run_vpa_gen_prefixed(VpaGenInput* input, const char* h_path, const char* ir_path, const char* prefix) {
  FILE* hf = fopen(h_path, "w");
  FILE* irf = fopen(ir_path, "w");
  assert(hf && irf);
  HeaderWriter* hw = hdwriter_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test_vpa.c", ".");
  vpa_gen(input, hw, w, prefix, 0);
  irwriter_end(w);

  hdwriter_del(hw);
  irwriter_del(w);
  fclose(irf);
  fclose(hf);
}

// === Review finding 1: {prefix}_parse / {prefix}_cleanup must be defined in IR ===

TEST(test_vpa_gen_prefix_parse_defined_in_ir) {
  VpaGenInput input = _empty_input();

  int32_t tok_a_id = symtab_intern(&input.tokens, "@tok_a");
  VpaScope main_scope = {.scope_id = 0, .name = strdup("main"), .leader = {0}};
  main_scope.children = darray_new(sizeof(VpaUnit), 0);
  VpaUnit u = {.kind = VPA_RE, .re = re_ir_new(), .action_units = _make_au_tok(tok_a_id)};
  u.re = re_ir_emit_ch(u.re, 'a');
  darray_push(main_scope.children, u);
  darray_push(input.scopes, main_scope);

  _run_vpa_gen_prefixed(&input, BUILD_DIR "/test_vpa_prefix.h", BUILD_DIR "/test_vpa_prefix.ll", "mymod");

  char* ir_buf = _read_file(BUILD_DIR "/test_vpa_prefix.ll");
  // The spec requires {prefix}_parse and {prefix}_cleanup to be defined (not just declared) in IR
  assert(strstr(ir_buf, "define") != NULL);
  assert(strstr(ir_buf, "@mymod_parse") != NULL);
  assert(strstr(ir_buf, "@mymod_cleanup") != NULL);
  free(ir_buf);

  _free_gen_input(&input);
}

// === Review finding 2: .begin/.end must push/pop scope in dispatch ===

TEST(test_vpa_gen_begin_end_push_pop) {
  VpaGenInput input = _empty_input();

  int32_t tok_x_id = symtab_intern(&input.tokens, "@tok_x");

  VpaScope main_scope = {.scope_id = 0, .name = strdup("main"), .leader = {0}};
  main_scope.children = darray_new(sizeof(VpaUnit), 0);

  VpaUnit u_x = {.kind = VPA_RE, .re = re_ir_new(), .action_units = _make_au_tok(tok_x_id)};
  u_x.re = re_ir_emit_ch(u_x.re, 'x');
  darray_push(main_scope.children, u_x);
  darray_push(input.scopes, main_scope);

  // child scope with leader '{'
  VpaScope child = {.scope_id = 1, .name = strdup("block")};
  child.leader = (VpaUnit){.kind = VPA_RE, .re = re_ir_new()};
  child.leader.re = re_ir_emit_ch(child.leader.re, '{');
  child.children = darray_new(sizeof(VpaUnit), 0);

  // child scope has an .end action on '}'
  VpaActionUnits end_au = darray_new(sizeof(int32_t), 0);
  int32_t end_hook = -HOOK_ID_END;
  darray_push(end_au, end_hook);
  VpaUnit u_close = {.kind = VPA_RE, .re = re_ir_new(), .action_units = end_au};
  u_close.re = re_ir_emit_ch(u_close.re, '}');
  darray_push(child.children, u_close);

  darray_push(input.scopes, child);

  // main calls child scope; action includes .begin
  VpaActionUnits call_au = darray_new(sizeof(int32_t), 0);
  int32_t begin_hook = -HOOK_ID_BEGIN;
  darray_push(call_au, begin_hook);
  VpaUnit call_u = {.kind = VPA_CALL, .call_scope_id = 1, .action_units = call_au};
  darray_push(input.scopes[0].children, call_u);

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_beginend.h", BUILD_DIR "/test_vpa_beginend.ll");

  char* ir_buf = _read_file(BUILD_DIR "/test_vpa_beginend.ll");
  // .begin action must actually call tt_push (not just declare it)
  assert(strstr(ir_buf, "call") != NULL);
  assert(strstr(ir_buf, "call i8* @tt_push") != NULL);
  // .end action must actually call tt_pop (not just declare it)
  assert(strstr(ir_buf, "call i8* @tt_pop") != NULL);
  free(ir_buf);

  _free_gen_input(&input);
}

// === Review finding 3: %effect must dispatch on user hook return value ===

TEST(test_vpa_gen_effect_dispatch) {
  VpaGenInput input = _empty_input();

  int32_t tok_a_id = symtab_intern(&input.tokens, "@tok_a");
  int32_t hook_check = symtab_intern(&input.hooks, ".check");

  // add effect declaration: .check can return tok_a or .fail
  EffectDecl ed;
  ed.hook_id = hook_check;
  ed.effects = darray_new(sizeof(int32_t), 0);
  darray_push(ed.effects, tok_a_id); // positive = token
  int32_t fail_au = -HOOK_ID_FAIL;
  darray_push(ed.effects, fail_au); // negative = hook
  darray_push(input.effect_decls, ed);

  VpaScope main_scope = {.scope_id = 0, .name = strdup("main"), .leader = {0}};
  main_scope.children = darray_new(sizeof(VpaUnit), 0);

  VpaActionUnits au = darray_new(sizeof(int32_t), 0);
  int32_t hook_au = -hook_check;
  darray_push(au, hook_au);
  VpaUnit u = {.kind = VPA_RE, .re = re_ir_new(), .action_units = au};
  u.re = re_ir_emit_ch(u.re, 'c');
  darray_push(main_scope.children, u);
  darray_push(input.scopes, main_scope);

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_effect.h", BUILD_DIR "/test_vpa_effect.ll");

  char* ir_buf = _read_file(BUILD_DIR "/test_vpa_effect.ll");
  // The hook call must happen
  assert(strstr(ir_buf, "call i32 @vpa_hook_check") != NULL);
  // The return value must be validated via switch (effect dispatch)
  assert(strstr(ir_buf, "switch") != NULL);
  free(ir_buf);

  _free_gen_input(&input);
}

// === Review finding 4: ParseErrorType / ParseError / ParseResult must be in header ===

TEST(test_vpa_gen_header_types) {
  VpaGenInput input = _empty_input();

  int32_t tok_a_id = symtab_intern(&input.tokens, "@tok_a");
  VpaScope main_scope = {.scope_id = 0, .name = strdup("main"), .leader = {0}};
  main_scope.children = darray_new(sizeof(VpaUnit), 0);
  VpaUnit u = {.kind = VPA_RE, .re = re_ir_new(), .action_units = _make_au_tok(tok_a_id)};
  u.re = re_ir_emit_ch(u.re, 'a');
  darray_push(main_scope.children, u);
  darray_push(input.scopes, main_scope);

  _run_vpa_gen_prefixed(&input, BUILD_DIR "/test_vpa_types.h", BUILD_DIR "/test_vpa_types.ll", "mymod");

  char* h_buf = _read_file(BUILD_DIR "/test_vpa_types.h");
  assert(strstr(h_buf, "ParseErrorType") != NULL);
  assert(strstr(h_buf, "PARSE_ERROR_INVALID_HOOK") != NULL);
  assert(strstr(h_buf, "PARSE_ERROR_TOKEN_ERR") != NULL);
  assert(strstr(h_buf, "ParseError") != NULL);
  assert(strstr(h_buf, "ParseResult") != NULL);

  // Spec: ParseResult must contain PegRef main field
  assert(strstr(h_buf, "PegRef") != NULL);
  assert(strstr(h_buf, "PegRef main") != NULL);

  free(h_buf);

  _free_gen_input(&input);
}

// === Spec check: {prefix}_parse must return ParseResult, not void ===

TEST(test_vpa_gen_parse_returns_parse_result) {
  VpaGenInput input = _empty_input();

  int32_t tok_a_id = symtab_intern(&input.tokens, "@tok_a");
  VpaScope main_scope = {.scope_id = 0, .name = strdup("main"), .leader = {0}};
  main_scope.children = darray_new(sizeof(VpaUnit), 0);
  VpaUnit u = {.kind = VPA_RE, .re = re_ir_new(), .action_units = _make_au_tok(tok_a_id)};
  u.re = re_ir_emit_ch(u.re, 'a');
  darray_push(main_scope.children, u);
  darray_push(input.scopes, main_scope);

  _run_vpa_gen_prefixed(&input, BUILD_DIR "/test_vpa_rettype.h", BUILD_DIR "/test_vpa_rettype.ll", "mymod");

  char* h_buf = _read_file(BUILD_DIR "/test_vpa_rettype.h");
  // Spec says: extern ParseResult {prefix}_parse(ParseContext $parse_context, UStr src)
  // Must NOT be "void mymod_parse"
  assert(strstr(h_buf, "void mymod_parse") == NULL);
  // Must declare returning ParseResult
  assert(strstr(h_buf, "ParseResult mymod_parse") != NULL);
  free(h_buf);

  _free_gen_input(&input);
}

// === Review finding 5: keyword literal tokens must be excluded from TOK_ defines ===

TEST(test_vpa_gen_literal_tokens_excluded) {
  VpaGenInput input = _empty_input();
  int32_t tok_id_id = symtab_intern(&input.tokens, "@ident");
  symtab_intern(&input.tokens, "@lit.if");
  symtab_intern(&input.tokens, "@lit.else");
  VpaScope main_scope = {.scope_id = 0, .name = strdup("main"), .leader = {0}};
  main_scope.children = darray_new(sizeof(VpaUnit), 0);
  VpaUnit u = {.kind = VPA_RE, .re = re_ir_new(), .action_units = _make_au_tok(tok_id_id)};
  u.re = re_ir_emit_ch(u.re, 'i');
  darray_push(main_scope.children, u);
  darray_push(input.scopes, main_scope);
  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_litincl.h", BUILD_DIR "/test_vpa_litincl.ll");
  char* h_buf = _read_file(BUILD_DIR "/test_vpa_litincl.h");
  assert(strstr(h_buf, "TOK_IDENT") != NULL);
  assert(strstr(h_buf, "TOK_LIT_IF") == NULL);
  assert(strstr(h_buf, "TOK_LIT_ELSE") == NULL);
  free(h_buf);
  _free_gen_input(&input);
}

// === Review finding 6: builtin hook IDs must be in header ===

TEST(test_vpa_gen_builtin_hook_defines) {
  VpaGenInput input = _empty_input();

  int32_t tok_a_id = symtab_intern(&input.tokens, "@tok_a");
  VpaScope main_scope = {.scope_id = 0, .name = strdup("main"), .leader = {0}};
  main_scope.children = darray_new(sizeof(VpaUnit), 0);
  VpaUnit u = {.kind = VPA_RE, .re = re_ir_new(), .action_units = _make_au_tok(tok_a_id)};
  u.re = re_ir_emit_ch(u.re, 'a');
  darray_push(main_scope.children, u);
  darray_push(input.scopes, main_scope);

  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_builtins.h", BUILD_DIR "/test_vpa_builtins.ll");

  char* h_buf = _read_file(BUILD_DIR "/test_vpa_builtins.h");
  // Only user-facing hook IDs emitted; BEGIN/END are internal
  assert(strstr(h_buf, "HOOK_BEGIN") == NULL);
  assert(strstr(h_buf, "HOOK_END") == NULL);
  assert(strstr(h_buf, "HOOK_FAIL") != NULL);
  assert(strstr(h_buf, "HOOK_UNPARSE") != NULL);
  free(h_buf);

  _free_gen_input(&input);
}

// === Scope switching: VPA must switch DFA when entering/leaving subscopes ===
// Grammar model:
//   main = { /a/ @tok_a   block   }
//   block = /{/ .begin @lbrace { /b/ @tok_b   /}/ @rbrace .end }
// Input: "a{bb}a"
// Expected token tree:
//   root (scope_id=main): [@tok_a, <scope_ref to block chunk>, @tok_a]
//   block chunk (scope_id=block): [@lbrace, @tok_b, @tok_b, @rbrace]

TEST(test_vpa_scope_switch_exec) {
  VpaGenInput input = _empty_input();

  int32_t tok_a_id = symtab_intern(&input.tokens, "@tok_a");
  int32_t tok_b_id = symtab_intern(&input.tokens, "@tok_b");
  int32_t tok_lbrace_id = symtab_intern(&input.tokens, "@lbrace");
  int32_t tok_rbrace_id = symtab_intern(&input.tokens, "@rbrace");

  // --- main scope (scope_id=0): matches 'a' and calls block ---
  VpaScope main_scope = {.scope_id = 0, .name = strdup("main"), .leader = {0}};
  main_scope.children = darray_new(sizeof(VpaUnit), 0);

  VpaUnit u_a = {.kind = VPA_RE, .re = re_ir_new(), .action_units = _make_au_tok(tok_a_id)};
  u_a.re = re_ir_emit_ch(u_a.re, 'a');
  darray_push(main_scope.children, u_a);

  // VPA_CALL to block — no action on the call itself (leader carries .begin)
  VpaUnit call_block = {.kind = VPA_CALL, .call_scope_id = 1, .action_units = NULL};
  darray_push(main_scope.children, call_block);

  darray_push(input.scopes, main_scope);

  // --- block scope (scope_id=1): leader = /{/ with .begin @lbrace ---
  VpaScope block_scope = {.scope_id = 1, .name = strdup("block"), .has_parser = false};
  block_scope.leader = (VpaUnit){.kind = VPA_RE, .re = re_ir_new()};
  block_scope.leader.re = re_ir_emit_ch(block_scope.leader.re, '{');
  // leader action: .begin then @lbrace
  block_scope.leader.action_units = darray_new(sizeof(int32_t), 0);
  int32_t begin_hook = -HOOK_ID_BEGIN;
  darray_push(block_scope.leader.action_units, begin_hook);
  darray_push(block_scope.leader.action_units, tok_lbrace_id);

  block_scope.children = darray_new(sizeof(VpaUnit), 0);

  // child pattern: /b/ @tok_b
  VpaUnit u_b = {.kind = VPA_RE, .re = re_ir_new(), .action_units = _make_au_tok(tok_b_id)};
  u_b.re = re_ir_emit_ch(u_b.re, 'b');
  darray_push(block_scope.children, u_b);

  // child pattern: /}/ @rbrace .end
  VpaActionUnits end_au = darray_new(sizeof(int32_t), 0);
  darray_push(end_au, tok_rbrace_id);
  int32_t end_hook = -HOOK_ID_END;
  darray_push(end_au, end_hook);
  VpaUnit u_close = {.kind = VPA_RE, .re = re_ir_new(), .action_units = end_au};
  u_close.re = re_ir_emit_ch(u_close.re, '}');
  darray_push(block_scope.children, u_close);

  darray_push(input.scopes, block_scope);

  // --- generate ---
  _run_vpa_gen(&input, BUILD_DIR "/test_vpa_scope_switch.h", BUILD_DIR "/test_vpa_scope_switch.ll");

  // --- compile driver that lexes "a{bb}a" and checks the token tree ---
  const char* driver_path = BUILD_DIR "/test_vpa_scope_switch_driver.c";
  FILE* df = fopen(driver_path, "w");
  assert(df);
  fprintf(
      df,
      "#include <assert.h>\n"
      "#include <stdint.h>\n"
      "#include <string.h>\n"
      "#include \"test_vpa_scope_switch.h\"\n"
      "\n"
      "int32_t vpa_rt_read_cp(void* src, int32_t cp_off) {\n"
      "  return ((const unsigned char*)src)[cp_off];\n"
      "}\n"
      "int32_t tt_depth(void* tt) { return 1; }\n"
      "struct { int64_t a; int64_t b; } parse_block(void* tt, void* sp) { return (typeof(parse_block(0,0))){0,0}; }\n"
      "\n"
      "int main(void) {\n"
      "  const char* input = \"a{bb}a\";\n"
      "  int32_t len = 6;\n"
      "  char* us = ustr_new(len, input);\n"
      "  TokenTree* tt = tt_tree_new(us);\n"
      "  vpa_lex((void*)input, len, (void*)tt, (void*)0, (void*)0);\n"
      "  /* root chunk: @tok_a, <scope ref>, @tok_a */\n"
      "  int32_t rn = (int32_t)darray_size(tt->root->tokens);\n"
      "  assert(rn == 3);\n"
      "  assert(tt->root->tokens[0].term_id == TOK_TOK_A);\n"
      "  assert(tt->root->tokens[0].cp_start == 0);\n"
      "  /* token[1] is a scope ref to the block chunk */\n"
      "  assert(tt->root->tokens[1].term_id == SCOPE_BLOCK);\n"
      "  int32_t cid = tt->root->tokens[1].chunk_id;\n"
      "  assert(cid >= 0);\n"
      "  /* last token after block */\n"
      "  assert(tt->root->tokens[2].term_id == TOK_TOK_A);\n"
      "  assert(tt->root->tokens[2].cp_start == 5);\n"
      "  /* block chunk: @lbrace @tok_b @tok_b @rbrace */\n"
      "  TokenChunk* bc = &tt->table[cid];\n"
      "  int32_t bn = (int32_t)darray_size(bc->tokens);\n"
      "  assert(bn == 4);\n"
      "  assert(bc->tokens[0].term_id == TOK_LBRACE);\n"
      "  assert(bc->tokens[1].term_id == TOK_TOK_B);\n"
      "  assert(bc->tokens[2].term_id == TOK_TOK_B);\n"
      "  assert(bc->tokens[3].term_id == TOK_RBRACE);\n"
      "  assert(bc->scope_id == SCOPE_BLOCK);\n"
      "  tt_tree_del(tt, false);\n"
      "  ustr_del(us);\n"
      "  return 0;\n"
      "}\n");
  fclose(df);

  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "%s %s %s -o %s 2>&1", compat_llvm_cc(), driver_path,
           BUILD_DIR "/test_vpa_scope_switch.ll", BUILD_DIR "/test_vpa_pop_exec_bin");
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
  RUN(test_vpa_gen_token_dedup);
  RUN(test_vpa_gen_empty_scope_body);
  RUN(test_vpa_gen_long_names);

  // review finding tests
  RUN(test_vpa_gen_prefix_parse_defined_in_ir);
  RUN(test_vpa_gen_begin_end_push_pop);
  RUN(test_vpa_gen_effect_dispatch);
  RUN(test_vpa_gen_header_types);
  RUN(test_vpa_gen_literal_tokens_excluded);
  RUN(test_vpa_gen_builtin_hook_defines);

  // spec conformance tests
  RUN(test_vpa_gen_parse_returns_parse_result);

  // scope switching tests
  RUN(test_vpa_scope_switch_exec);

  printf("all ok\n");
  return 0;
}
