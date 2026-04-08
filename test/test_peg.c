#include "../src/darray.h"
#include "../src/header_writer.h"
#include "../src/irwriter.h"
#include "../src/peg.h"
#include "../src/symtab.h"
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

TEST(test_empty_input) {
  Symtab tokens = {0};
  symtab_init(&tokens, 0);
  Symtab rule_names = {0};
  symtab_init(&rule_names, 0);
  Symtab scope_names = {0};
  symtab_init(&scope_names, 0);

  PegGenInput input = {0};
  input.rules = darray_new(sizeof(PegRule), 0);
  input.tokens = tokens;
  input.rule_names = rule_names;
  input.scope_names = scope_names;

  FILE* hf = fopen(BUILD_DIR "/test_peg_empty.h", "w");
  FILE* irf = fopen(BUILD_DIR "/test_peg_empty.ll", "w");
  assert(hf && irf);
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test.c", ".");
  peg_gen(&input, hw, w, false, "test");
  irwriter_end(w);

  hw_del(hw);
  irwriter_del(w);
  fclose(irf);
  fclose(hf);

  char* ir_buf;
  size_t ir_sz;
  FILE* ir_read = fopen(BUILD_DIR "/test_peg_empty.ll", "r");
  assert(ir_read);
  fseek(ir_read, 0, SEEK_END);
  ir_sz = (size_t)ftell(ir_read);
  rewind(ir_read);
  ir_buf = malloc(ir_sz + 1);
  fread(ir_buf, 1, ir_sz, ir_read);
  ir_buf[ir_sz] = '\0';
  fclose(ir_read);

  assert(strstr(ir_buf, "source_filename") != NULL);
  assert(strstr(ir_buf, "parse_") == NULL);

  free(ir_buf);
  _compile_test(BUILD_DIR "/test_peg_empty.h", BUILD_DIR "/test_peg_empty.ll");
  darray_del(input.rules);
  symtab_free(&tokens);
  symtab_free(&rule_names);
  symtab_free(&scope_names);
}

TEST(test_simple_rule_naive) {
  Symtab tokens = {0};
  symtab_init(&tokens, 0);
  Symtab rule_names = {0};
  symtab_init(&rule_names, 0);
  Symtab scope_names = {0};
  symtab_init(&scope_names, 0);

  int32_t tok_NUM = symtab_intern(&tokens, "NUM");
  int32_t rule_expr = symtab_intern(&rule_names, "expr");

  PegGenInput input = {0};
  input.rules = darray_new(sizeof(PegRule), 0);
  input.tokens = tokens;
  input.rule_names = rule_names;
  input.scope_names = scope_names;

  PegRule rule = {0};
  rule.global_id = rule_expr;
  rule.scope_id = -1;
  rule.seq.kind = PEG_SEQ;
  rule.seq.children = darray_new(sizeof(PegUnit), 0);

  PegUnit tok = {0};
  tok.kind = PEG_TOK;
  tok.id = tok_NUM;
  darray_push(rule.seq.children, tok);

  darray_push(input.rules, rule);

  FILE* hf = fopen(BUILD_DIR "/test_peg_naive.h", "w");
  FILE* irf = fopen(BUILD_DIR "/test_peg_naive.ll", "w");
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test.c", ".");
  peg_gen(&input, hw, w, false, "test");
  irwriter_end(w);

  hw_del(hw);
  irwriter_del(w);
  fclose(irf);
  fclose(hf);

  FILE* check = fopen(BUILD_DIR "/test_peg_naive.h", "r");
  char buf[1024];
  int found_ref = 0, found_node = 0, found_col = 0;
  while (fgets(buf, sizeof(buf), check)) {
    if (strstr(buf, "PegRef"))
      found_ref = 1;
    if (strstr(buf, "ExprNode"))
      found_node = 1;
    if (strstr(buf, "Col_main"))
      found_col = 1;
  }
  fclose(check);
  assert(found_ref);
  assert(found_node);
  assert(found_col);

  _compile_test(BUILD_DIR "/test_peg_naive.h", BUILD_DIR "/test_peg_naive.ll");

  darray_del(rule.seq.children);
  darray_del(input.rules);
  symtab_free(&tokens);
  symtab_free(&rule_names);
  symtab_free(&scope_names);
}

TEST(test_row_shared_mode) {
  Symtab tokens = {0};
  symtab_init(&tokens, 0);
  Symtab rule_names = {0};
  symtab_init(&rule_names, 0);
  Symtab scope_names = {0};
  symtab_init(&scope_names, 0);

  int32_t tok_A = symtab_intern(&tokens, "A");
  int32_t tok_B = symtab_intern(&tokens, "B");
  int32_t rule_a = symtab_intern(&rule_names, "a");
  int32_t rule_b = symtab_intern(&rule_names, "b");

  PegGenInput input = {0};
  input.rules = darray_new(sizeof(PegRule), 0);
  input.tokens = tokens;
  input.rule_names = rule_names;
  input.scope_names = scope_names;

  PegRule r1 = {0};
  r1.global_id = rule_a;
  r1.scope_id = -1;
  r1.seq.kind = PEG_SEQ;
  r1.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t1 = {.kind = PEG_TOK, .id = tok_A};
  darray_push(r1.seq.children, t1);
  darray_push(input.rules, r1);

  PegRule r2 = {0};
  r2.global_id = rule_b;
  r2.scope_id = -1;
  r2.seq.kind = PEG_SEQ;
  r2.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t2 = {.kind = PEG_TOK, .id = tok_B};
  darray_push(r2.seq.children, t2);
  darray_push(input.rules, r2);

  FILE* hf = fopen(BUILD_DIR "/test_peg_shared.h", "w");
  FILE* irf = fopen(BUILD_DIR "/test_peg_shared.ll", "w");
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test.c", ".");
  peg_gen(&input, hw, w, true, "test");
  irwriter_end(w);

  hw_del(hw);
  irwriter_del(w);
  fclose(irf);
  fclose(hf);

  FILE* check = fopen(BUILD_DIR "/test_peg_shared.h", "r");
  char buf[1024];
  int found_bits = 0;
  while (fgets(buf, sizeof(buf), check)) {
    if (strstr(buf, "bits["))
      found_bits = 1;
  }
  fclose(check);
  assert(found_bits);

  _compile_test(BUILD_DIR "/test_peg_shared.h", BUILD_DIR "/test_peg_shared.ll");

  darray_del(r1.seq.children);
  darray_del(r2.seq.children);
  darray_del(input.rules);
  symtab_free(&tokens);
  symtab_free(&rule_names);
  symtab_free(&scope_names);
}

TEST(test_branch_rule) {
  // Rule: foo = [ @A :tag1 | @B :tag2 ]
  Symtab tokens = {0};
  symtab_init(&tokens, 0);
  Symtab rule_names = {0};
  symtab_init(&rule_names, 0);
  Symtab scope_names = {0};
  symtab_init(&scope_names, 0);

  int32_t tok_A = symtab_intern(&tokens, "A");
  int32_t tok_B = symtab_intern(&tokens, "B");
  int32_t rule_foo = symtab_intern(&rule_names, "foo");

  PegGenInput input = {0};
  input.rules = darray_new(sizeof(PegRule), 0);
  input.tokens = tokens;
  input.rule_names = rule_names;
  input.scope_names = scope_names;

  PegRule rule = {0};
  rule.global_id = rule_foo;
  rule.scope_id = -1;
  rule.seq.kind = PEG_SEQ;
  rule.seq.children = darray_new(sizeof(PegUnit), 0);

  PegUnit branches = {0};
  branches.kind = PEG_BRANCHES;
  branches.children = darray_new(sizeof(PegUnit), 0);

  PegUnit b1 = {0};
  b1.kind = PEG_SEQ;
  b1.tag = strdup("tag1");
  b1.children = darray_new(sizeof(PegUnit), 0);
  PegUnit bt1 = {.kind = PEG_TOK, .id = tok_A};
  darray_push(b1.children, bt1);
  darray_push(branches.children, b1);

  PegUnit b2 = {0};
  b2.kind = PEG_SEQ;
  b2.tag = strdup("tag2");
  b2.children = darray_new(sizeof(PegUnit), 0);
  PegUnit bt2 = {.kind = PEG_TOK, .id = tok_B};
  darray_push(b2.children, bt2);
  darray_push(branches.children, b2);

  darray_push(rule.seq.children, branches);
  darray_push(input.rules, rule);

  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  FILE* hf = compat_open_memstream(&hdr_buf, &hdr_sz);
  FILE* irf = fopen(BUILD_DIR "/test_peg_branch.ll", "w");
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test.c", ".");
  peg_gen(&input, hw, w, false, "test");
  irwriter_end(w);

  hw_del(hw);
  irwriter_del(w);
  fclose(irf);
  compat_close_memstream(hf, &hdr_buf, &hdr_sz);

  // Check: node type has `is` bitfield with tag1 and tag2
  assert(strstr(hdr_buf, "bool tag1 : 1"));
  assert(strstr(hdr_buf, "bool tag2 : 1"));
  // Check: load function reads slot and extracts branch_id
  assert(strstr(hdr_buf, "branch_id"));
  assert(strstr(hdr_buf, "node.is.tag1"));
  assert(strstr(hdr_buf, "node.is.tag2"));
  // Check: per-scope Col type
  assert(strstr(hdr_buf, "Col_main"));

  free(hdr_buf);

  // Cleanup
  free(input.rules[0].seq.children[0].children[0].tag);
  darray_del(input.rules[0].seq.children[0].children[0].children);
  free(input.rules[0].seq.children[0].children[1].tag);
  darray_del(input.rules[0].seq.children[0].children[1].children);
  darray_del(input.rules[0].seq.children[0].children);
  darray_del(input.rules[0].seq.children);
  darray_del(input.rules);
  symtab_free(&tokens);
  symtab_free(&rule_names);
  symtab_free(&scope_names);
}

TEST(test_per_scope_col) {
  // Two rules in different scopes should get different Col types
  Symtab tokens = {0};
  symtab_init(&tokens, 0);
  Symtab rule_names = {0};
  symtab_init(&rule_names, 0);
  Symtab scope_names = {0};
  symtab_init(&scope_names, 0);

  int32_t tok_X = symtab_intern(&tokens, "X");
  int32_t tok_Y = symtab_intern(&tokens, "Y");
  int32_t tok_Z = symtab_intern(&tokens, "Z");
  int32_t rule_a = symtab_intern(&rule_names, "a");
  int32_t rule_b = symtab_intern(&rule_names, "b");
  int32_t rule_c = symtab_intern(&rule_names, "c");
  int32_t scope_s1 = symtab_intern(&scope_names, "s1");
  int32_t scope_s2 = symtab_intern(&scope_names, "s2");

  PegGenInput input = {0};
  input.rules = darray_new(sizeof(PegRule), 0);
  input.tokens = tokens;
  input.rule_names = rule_names;
  input.scope_names = scope_names;

  PegRule r1 = {0};
  r1.global_id = rule_a;
  r1.scope_id = scope_s1;
  r1.seq.kind = PEG_SEQ;
  r1.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t1 = {.kind = PEG_TOK, .id = tok_X};
  darray_push(r1.seq.children, t1);
  darray_push(input.rules, r1);

  PegRule r2 = {0};
  r2.global_id = rule_b;
  r2.scope_id = scope_s1;
  r2.seq.kind = PEG_SEQ;
  r2.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t2 = {.kind = PEG_TOK, .id = tok_Y};
  darray_push(r2.seq.children, t2);
  darray_push(input.rules, r2);

  PegRule r3 = {0};
  r3.global_id = rule_c;
  r3.scope_id = scope_s2;
  r3.seq.kind = PEG_SEQ;
  r3.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t3 = {.kind = PEG_TOK, .id = tok_Z};
  darray_push(r3.seq.children, t3);
  darray_push(input.rules, r3);

  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  FILE* hf = compat_open_memstream(&hdr_buf, &hdr_sz);
  FILE* irf = fopen(BUILD_DIR "/test_peg_scope.ll", "w");
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test.c", ".");
  peg_gen(&input, hw, w, false, "test");
  irwriter_end(w);

  hw_del(hw);
  irwriter_del(w);
  fclose(irf);
  compat_close_memstream(hf, &hdr_buf, &hdr_sz);

  // Check: separate Col types per scope
  assert(strstr(hdr_buf, "Col_s1"));
  assert(strstr(hdr_buf, "Col_s2"));
  // s1 has 2 rules → slots[2], s2 has 1 rule → slots[1]
  assert(strstr(hdr_buf, "slots[2]"));
  assert(strstr(hdr_buf, "slots[1]"));

  free(hdr_buf);

  darray_del(r1.seq.children);
  darray_del(r2.seq.children);
  darray_del(r3.seq.children);
  darray_del(input.rules);
  symtab_free(&tokens);
  symtab_free(&rule_names);
  symtab_free(&scope_names);
}

TEST(test_row_shared_per_scope_compact) {
  Symtab tokens = {0};
  symtab_init(&tokens, 0);
  Symtab rule_names = {0};
  symtab_init(&rule_names, 0);
  Symtab scope_names = {0};
  symtab_init(&scope_names, 0);

  int32_t tok_X = symtab_intern(&tokens, "X");
  int32_t tok_Y = symtab_intern(&tokens, "Y");
  int32_t rule_a = symtab_intern(&rule_names, "a");
  int32_t rule_b = symtab_intern(&rule_names, "b");
  int32_t rule_c = symtab_intern(&rule_names, "c");
  int32_t scope_s1 = symtab_intern(&scope_names, "s1");
  int32_t scope_s2 = symtab_intern(&scope_names, "s2");

  PegGenInput input = {0};
  input.rules = darray_new(sizeof(PegRule), 0);
  input.tokens = tokens;
  input.rule_names = rule_names;
  input.scope_names = scope_names;

  PegRule r1 = {0};
  r1.global_id = rule_a;
  r1.scope_id = scope_s1;
  r1.seq.kind = PEG_SEQ;
  r1.seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(r1.seq.children, ((PegUnit){.kind = PEG_TOK, .id = tok_X}));
  darray_push(input.rules, r1);

  PegRule r2 = {0};
  r2.global_id = rule_b;
  r2.scope_id = scope_s1;
  r2.seq.kind = PEG_SEQ;
  r2.seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(r2.seq.children, ((PegUnit){.kind = PEG_TOK, .id = tok_X}));
  darray_push(input.rules, r2);

  PegRule r3 = {0};
  r3.global_id = rule_c;
  r3.scope_id = scope_s2;
  r3.seq.kind = PEG_SEQ;
  r3.seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(r3.seq.children, ((PegUnit){.kind = PEG_TOK, .id = tok_Y}));
  darray_push(input.rules, r3);

  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  FILE* hf = compat_open_memstream(&hdr_buf, &hdr_sz);
  FILE* irf = fopen(BUILD_DIR "/test_peg_shared_scope_compact.ll", "w");
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test.c", ".");
  peg_gen(&input, hw, w, true, "test");
  irwriter_end(w);

  hw_del(hw);
  irwriter_del(w);
  fclose(irf);
  compat_close_memstream(hf, &hdr_buf, &hdr_sz);

  assert(strstr(hdr_buf, "Col_s1"));
  assert(strstr(hdr_buf, "Col_s2"));
  assert(strstr(hdr_buf, "int32_t slots[2];"));
  assert(strstr(hdr_buf, "int32_t slots[1];"));

  free(hdr_buf);

  darray_del(r1.seq.children);
  darray_del(r2.seq.children);
  darray_del(r3.seq.children);
  darray_del(input.rules);
  symtab_free(&tokens);
  symtab_free(&rule_names);
  symtab_free(&scope_names);
}

TEST(test_scope_refs_not_expanded_in_sets) {
  Symtab tokens = {0};
  symtab_init(&tokens, 0);
  Symtab rule_names = {0};
  symtab_init(&rule_names, 0);
  Symtab scope_names = {0};
  symtab_init(&scope_names, 0);

  int32_t tok_A = symtab_intern(&tokens, "A");
  int32_t rule_start = symtab_intern(&rule_names, "start");
  int32_t rule_tok_rule = symtab_intern(&rule_names, "tok_rule");
  int32_t rule_inner = symtab_intern(&rule_names, "inner");
  int32_t scope_inner = symtab_intern(&scope_names, "inner");

  PegGenInput input = {0};
  input.rules = darray_new(sizeof(PegRule), 0);
  input.tokens = tokens;
  input.rule_names = rule_names;
  input.scope_names = scope_names;

  PegRule start = {0};
  start.global_id = rule_start;
  start.scope_id = -1;
  start.seq.kind = PEG_SEQ;
  start.seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(start.seq.children, ((PegUnit){.kind = PEG_CALL, .id = rule_inner}));
  darray_push(input.rules, start);

  PegRule tok = {0};
  tok.global_id = rule_tok_rule;
  tok.scope_id = -1;
  tok.seq.kind = PEG_SEQ;
  tok.seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(tok.seq.children, ((PegUnit){.kind = PEG_TOK, .id = tok_A}));
  darray_push(input.rules, tok);

  PegRule inner = {0};
  inner.global_id = rule_inner;
  inner.scope_id = scope_inner;
  inner.seq.kind = PEG_SEQ;
  inner.seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(inner.seq.children, ((PegUnit){.kind = PEG_TOK, .id = tok_A}));
  darray_push(input.rules, inner);

  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  FILE* hf = compat_open_memstream(&hdr_buf, &hdr_sz);
  FILE* irf = fopen(BUILD_DIR "/test_peg_scope_ref_sets.ll", "w");
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test.c", ".");
  peg_gen(&input, hw, w, true, "test");
  irwriter_end(w);

  hw_del(hw);
  irwriter_del(w);
  fclose(irf);
  compat_close_memstream(hf, &hdr_buf, &hdr_sz);

  assert(strstr(hdr_buf, "Col_main"));
  assert(strstr(hdr_buf, "int32_t slots[1];"));

  free(hdr_buf);

  darray_del(start.seq.children);
  darray_del(tok.seq.children);
  darray_del(inner.seq.children);
  darray_del(input.rules);
  symtab_free(&tokens);
  symtab_free(&rule_names);
  symtab_free(&scope_names);
}

int main(void) {
  printf("test_peg:\n");
  RUN(test_empty_input);
  RUN(test_simple_rule_naive);
  RUN(test_row_shared_mode);
  RUN(test_branch_rule);
  RUN(test_per_scope_col);
  RUN(test_row_shared_per_scope_compact);
  RUN(test_scope_refs_not_expanded_in_sets);
  printf("All tests passed.\n");
  return 0;
}
