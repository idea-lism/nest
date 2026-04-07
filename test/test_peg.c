#include "../src/darray.h"
#include "../src/header_writer.h"
#include "../src/irwriter.h"
#include "../src/peg.h"
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

  FILE* hf = fopen(BUILD_DIR "/test_peg_empty.h", "w");
  FILE* irf = fopen(BUILD_DIR "/test_peg_empty.ll", "w");
  assert(hf && irf);
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test.c", ".");
  peg_gen(&input, hw, w, false);
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
}

TEST(test_simple_rule_naive) {
  PegGenInput input = {0};
  input.rules = darray_new(sizeof(PegRule), 0);

  PegRule rule = {0};
  rule.name = strdup("expr");
  rule.seq.kind = PEG_SEQ;
  rule.seq.children = darray_new(sizeof(PegUnit), 0);

  PegUnit tok = {0};
  tok.kind = PEG_TOK;
  tok.name = strdup("NUM");
  darray_push(rule.seq.children, tok);

  darray_push(input.rules, rule);

  FILE* hf = fopen(BUILD_DIR "/test_peg_naive.h", "w");
  FILE* irf = fopen(BUILD_DIR "/test_peg_naive.ll", "w");
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test.c", ".");
  peg_gen(&input, hw, w, false);
  irwriter_end(w);

  hw_del(hw);
  irwriter_del(w);
  fclose(irf);
  fclose(hf);

  FILE* check = fopen(BUILD_DIR "/test_peg_naive.h", "r");
  char buf[1024];
  int found_ref = 0, found_node = 0, found_col = 0;
  while (fgets(buf, sizeof(buf), check)) {
    if (strstr(buf, "PegRef")) {
      found_ref = 1;
    }
    if (strstr(buf, "ExprNode")) {
      found_node = 1;
    }
    if (strstr(buf, "Col_main")) {
      found_col = 1;
    }
  }
  fclose(check);
  assert(found_ref);
  assert(found_node);
  assert(found_col);

  _compile_test(BUILD_DIR "/test_peg_naive.h", BUILD_DIR "/test_peg_naive.ll");

  free(rule.seq.children[0].name);
  darray_del(rule.seq.children);
  free(rule.name);
  darray_del(input.rules);
}

TEST(test_row_shared_mode) {
  PegGenInput input = {0};
  input.rules = darray_new(sizeof(PegRule), 0);

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

  FILE* hf = fopen(BUILD_DIR "/test_peg_shared.h", "w");
  FILE* irf = fopen(BUILD_DIR "/test_peg_shared.ll", "w");
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test.c", ".");
  peg_gen(&input, hw, w, true);
  irwriter_end(w);

  hw_del(hw);
  irwriter_del(w);
  fclose(irf);
  fclose(hf);

  FILE* check = fopen(BUILD_DIR "/test_peg_shared.h", "r");
  char buf[1024];
  int found_bits = 0;
  while (fgets(buf, sizeof(buf), check)) {
    if (strstr(buf, "bits[")) {
      found_bits = 1;
    }
  }
  fclose(check);
  assert(found_bits);

  _compile_test(BUILD_DIR "/test_peg_shared.h", BUILD_DIR "/test_peg_shared.ll");

  free(r1.seq.children[0].name);
  darray_del(r1.seq.children);
  free(r1.name);
  free(r2.seq.children[0].name);
  darray_del(r2.seq.children);
  free(r2.name);
  darray_del(input.rules);
}

TEST(test_branch_rule) {
  // Rule: foo = [ @A :tag1 | @B :tag2 ]
  PegGenInput input = {0};
  input.rules = darray_new(sizeof(PegRule), 0);

  PegRule rule = {0};
  rule.name = strdup("foo");
  rule.seq.kind = PEG_SEQ;
  rule.seq.children = darray_new(sizeof(PegUnit), 0);

  PegUnit branches = {0};
  branches.kind = PEG_BRANCHES;
  branches.children = darray_new(sizeof(PegUnit), 0);

  PegUnit b1 = {0};
  b1.kind = PEG_SEQ;
  b1.tag = strdup("tag1");
  b1.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t1 = {.kind = PEG_TOK, .name = strdup("A")};
  darray_push(b1.children, t1);
  darray_push(branches.children, b1);

  PegUnit b2 = {0};
  b2.kind = PEG_SEQ;
  b2.tag = strdup("tag2");
  b2.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t2 = {.kind = PEG_TOK, .name = strdup("B")};
  darray_push(b2.children, t2);
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
  peg_gen(&input, hw, w, false);
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
  // Check: per-scope alloc/free
  assert(strstr(hdr_buf, "peg_alloc_main"));
  assert(strstr(hdr_buf, "peg_free_main"));

  free(hdr_buf);

  // Cleanup
  free(input.rules[0].seq.children[0].children[0].children[0].name);
  darray_del(input.rules[0].seq.children[0].children[0].children);
  free(input.rules[0].seq.children[0].children[0].tag);
  free(input.rules[0].seq.children[0].children[1].children[0].name);
  darray_del(input.rules[0].seq.children[0].children[1].children);
  free(input.rules[0].seq.children[0].children[1].tag);
  darray_del(input.rules[0].seq.children[0].children);
  darray_del(input.rules[0].seq.children);
  free(input.rules[0].name);
  darray_del(input.rules);
}

TEST(test_per_scope_col) {
  // Two rules in different scopes should get different Col types
  PegGenInput input = {0};
  input.rules = darray_new(sizeof(PegRule), 0);

  PegRule r1 = {0};
  r1.name = strdup("a");
  r1.scope = strdup("s1");
  r1.seq.kind = PEG_SEQ;
  r1.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t1 = {.kind = PEG_TOK, .name = strdup("X")};
  darray_push(r1.seq.children, t1);
  darray_push(input.rules, r1);

  PegRule r2 = {0};
  r2.name = strdup("b");
  r2.scope = strdup("s1");
  r2.seq.kind = PEG_SEQ;
  r2.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t2 = {.kind = PEG_TOK, .name = strdup("Y")};
  darray_push(r2.seq.children, t2);
  darray_push(input.rules, r2);

  PegRule r3 = {0};
  r3.name = strdup("c");
  r3.scope = strdup("s2");
  r3.seq.kind = PEG_SEQ;
  r3.seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t3 = {.kind = PEG_TOK, .name = strdup("Z")};
  darray_push(r3.seq.children, t3);
  darray_push(input.rules, r3);

  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  FILE* hf = compat_open_memstream(&hdr_buf, &hdr_sz);
  FILE* irf = fopen(BUILD_DIR "/test_peg_scope.ll", "w");
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test.c", ".");
  peg_gen(&input, hw, w, false);
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
  // Per-scope alloc/free
  assert(strstr(hdr_buf, "peg_alloc_s1"));
  assert(strstr(hdr_buf, "peg_alloc_s2"));

  free(hdr_buf);

  free(r1.seq.children[0].name);
  darray_del(r1.seq.children);
  free(r1.name);
  free(r1.scope);
  free(r2.seq.children[0].name);
  darray_del(r2.seq.children);
  free(r2.name);
  free(r2.scope);
  free(r3.seq.children[0].name);
  darray_del(r3.seq.children);
  free(r3.name);
  free(r3.scope);
  darray_del(input.rules);
}

TEST(test_row_shared_per_scope_compact) {
  PegGenInput input = {0};
  input.rules = darray_new(sizeof(PegRule), 0);

  PegRule r1 = {0};
  r1.name = strdup("a");
  r1.scope = strdup("s1");
  r1.seq.kind = PEG_SEQ;
  r1.seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(r1.seq.children, ((PegUnit){.kind = PEG_TOK, .name = strdup("X")}));
  darray_push(input.rules, r1);

  PegRule r2 = {0};
  r2.name = strdup("b");
  r2.scope = strdup("s1");
  r2.seq.kind = PEG_SEQ;
  r2.seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(r2.seq.children, ((PegUnit){.kind = PEG_TOK, .name = strdup("X")}));
  darray_push(input.rules, r2);

  PegRule r3 = {0};
  r3.name = strdup("c");
  r3.scope = strdup("s2");
  r3.seq.kind = PEG_SEQ;
  r3.seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(r3.seq.children, ((PegUnit){.kind = PEG_TOK, .name = strdup("Y")}));
  darray_push(input.rules, r3);

  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  FILE* hf = compat_open_memstream(&hdr_buf, &hdr_sz);
  FILE* irf = fopen(BUILD_DIR "/test_peg_shared_scope_compact.ll", "w");
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test.c", ".");
  peg_gen(&input, hw, w, true);
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

  free(r1.seq.children[0].name);
  darray_del(r1.seq.children);
  free(r1.name);
  free(r1.scope);
  free(r2.seq.children[0].name);
  darray_del(r2.seq.children);
  free(r2.name);
  free(r2.scope);
  free(r3.seq.children[0].name);
  darray_del(r3.seq.children);
  free(r3.name);
  free(r3.scope);
  darray_del(input.rules);
}

TEST(test_scope_refs_not_expanded_in_sets) {
  PegGenInput input = {0};
  input.rules = darray_new(sizeof(PegRule), 0);

  PegRule start = {0};
  start.name = strdup("start");
  start.seq.kind = PEG_SEQ;
  start.seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(start.seq.children, ((PegUnit){.kind = PEG_ID, .name = strdup("inner")}));
  darray_push(input.rules, start);

  PegRule tok = {0};
  tok.name = strdup("tok_rule");
  tok.seq.kind = PEG_SEQ;
  tok.seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(tok.seq.children, ((PegUnit){.kind = PEG_TOK, .name = strdup("A")}));
  darray_push(input.rules, tok);

  PegRule inner = {0};
  inner.name = strdup("inner");
  inner.scope = strdup("inner");
  inner.seq.kind = PEG_SEQ;
  inner.seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(inner.seq.children, ((PegUnit){.kind = PEG_TOK, .name = strdup("A")}));
  darray_push(input.rules, inner);

  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  FILE* hf = compat_open_memstream(&hdr_buf, &hdr_sz);
  FILE* irf = fopen(BUILD_DIR "/test_peg_scope_ref_sets.ll", "w");
  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, NULL);

  irwriter_start(w, "test.c", ".");
  peg_gen(&input, hw, w, true);
  irwriter_end(w);

  hw_del(hw);
  irwriter_del(w);
  fclose(irf);
  compat_close_memstream(hf, &hdr_buf, &hdr_sz);

  assert(strstr(hdr_buf, "Col_main"));
  assert(strstr(hdr_buf, "int32_t slots[1];"));

  free(hdr_buf);

  free(start.seq.children[0].name);
  darray_del(start.seq.children);
  free(start.name);
  free(tok.seq.children[0].name);
  darray_del(tok.seq.children);
  free(tok.name);
  free(inner.seq.children[0].name);
  darray_del(inner.seq.children);
  free(inner.name);
  free(inner.scope);
  darray_del(input.rules);
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
