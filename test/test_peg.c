#include "../src/peg.h"
#include "../src/darray.h"
#include "../src/header_writer.h"
#include "../src/irwriter.h"
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
  PegGenInput input = {0};
  input.rules = darray_new(sizeof(PegRule), 0);
  input.mode = PEG_MODE_NAIVE;
  input.token_ids = NULL;
  input.n_tokens = 0;

  FILE* hf = fopen("build/debug/test_peg_empty.h", "w");
  FILE* irf = fopen("build/debug/test_peg_empty.ll", "w");
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, "x86_64-unknown-linux-gnu");

  irwriter_start(w, "test.c", ".");
  peg_gen(&input, hw, w);
  irwriter_end(w);

  hw_del(hw);
  irwriter_del(w);
  fclose(irf);
  fclose(hf);
  darray_del(input.rules);
}

TEST(test_simple_rule_naive) {
  PegGenInput input = {0};
  input.rules = darray_new(sizeof(PegRule), 0);
  input.mode = PEG_MODE_NAIVE;
  input.token_ids = NULL;
  input.n_tokens = 0;

  PegRule rule = {0};
  rule.name = strdup("expr");
  rule.seq.kind = PEG_SEQ;
  rule.seq.children = darray_new(sizeof(PegUnit), 0);
  
  PegUnit tok = {0};
  tok.kind = PEG_TOK;
  tok.name = strdup("NUM");
  darray_push(rule.seq.children, tok);
  
  darray_push(input.rules, rule);

  FILE* hf = fopen("build/debug/test_peg_naive.h", "w");
  FILE* irf = fopen("build/debug/test_peg_naive.ll", "w");
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, "x86_64-unknown-linux-gnu");

  irwriter_start(w, "test.c", ".");
  peg_gen(&input, hw, w);
  irwriter_end(w);

  hw_del(hw);
  irwriter_del(w);
  fclose(irf);
  fclose(hf);
  
  FILE* check = fopen("build/debug/test_peg_naive.h", "r");
  char buf[1024];
  int found_ref = 0, found_node = 0, found_col = 0;
  while (fgets(buf, sizeof(buf), check)) {
    if (strstr(buf, "PegRef")) found_ref = 1;
    if (strstr(buf, "ExprNode")) found_node = 1;
    if (strstr(buf, "Col")) found_col = 1;
  }
  fclose(check);
  assert(found_ref);
  assert(found_node);
  assert(found_col);
  
  _compile_test("build/debug/test_peg_naive.h", "build/debug/test_peg_naive.ll");
  
  free(rule.seq.children[0].name);
  darray_del(rule.seq.children);
  free(rule.name);
  darray_del(input.rules);
}

TEST(test_row_shared_mode) {
  PegGenInput input = {0};
  input.rules = darray_new(sizeof(PegRule), 0);
  input.mode = PEG_MODE_ROW_SHARED;
  input.token_ids = NULL;
  input.n_tokens = 0;

  PegRule r1 = {0};
  r1.name = strdup("a");
  r1.seq.kind = PEG_SEQ;
  r1.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t1 = {0};
  t1.kind = PEG_TOK;
  t1.name = strdup("A");
  darray_push(r1.seq.children, t1);
  darray_push(input.rules, r1);

  PegRule r2 = {0};
  r2.name = strdup("b");
  r2.seq.kind = PEG_SEQ;
  r2.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t2 = {0};
  t2.kind = PEG_TOK;
  t2.name = strdup("B");
  darray_push(r2.seq.children, t2);
  darray_push(input.rules, r2);

  FILE* hf = fopen("build/debug/test_peg_shared.h", "w");
  FILE* irf = fopen("build/debug/test_peg_shared.ll", "w");
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, "x86_64-unknown-linux-gnu");

  irwriter_start(w, "test.c", ".");
  peg_gen(&input, hw, w);
  irwriter_end(w);

  hw_del(hw);
  irwriter_del(w);
  fclose(irf);
  fclose(hf);

  FILE* check = fopen("build/debug/test_peg_shared.h", "r");
  char buf[1024];
  int found_bits = 0;
  while (fgets(buf, sizeof(buf), check)) {
    if (strstr(buf, "bits[")) found_bits = 1;
  }
  fclose(check);
  assert(found_bits);

  _compile_test("build/debug/test_peg_shared.h", "build/debug/test_peg_shared.ll");

  free(r1.seq.children[0].name);
  darray_del(r1.seq.children);
  free(r1.name);
  free(r2.seq.children[0].name);
  darray_del(r2.seq.children);
  free(r2.name);
  darray_del(input.rules);
}

int main(void) {
  printf("test_peg:\n");
  RUN(test_empty_input);
  RUN(test_simple_rule_naive);
  RUN(test_row_shared_mode);
  printf("All tests passed.\n");
  return 0;
}

