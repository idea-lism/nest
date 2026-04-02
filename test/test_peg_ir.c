#include "../src/darray.h"
#include "../src/irwriter.h"
#include "../src/peg.h"
#include "../src/peg_ir.h"
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

static char* _capture_ir(void (*emit)(IrWriter*)) {
  char* buf = NULL;
  size_t buf_sz = 0;
  FILE* f = compat_open_memstream(&buf, &buf_sz);
  assert(f);
  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "test_peg_ir.c", ".");

  emit(w);

  irwriter_end(w);
  irwriter_del(w);
  compat_close_memstream(f, &buf, &buf_sz);
  return buf;
}

// --- peg_ir_emit_bt_defs ---

static void _emit_bt_defs(IrWriter* w) { peg_ir_emit_bt_defs(w); }

TEST(test_bt_defs) {
  char* ir = _capture_ir(_emit_bt_defs);
  assert(strstr(ir, "%BtStack") != NULL);
  assert(strstr(ir, "@save") != NULL);
  assert(strstr(ir, "@restore") != NULL);
  assert(strstr(ir, "@discard") != NULL);
  free(ir);
}

// --- peg_ir_declare_externs ---

static void _emit_externs(IrWriter* w) { peg_ir_declare_externs(w); }

TEST(test_declare_externs) {
  char* ir = _capture_ir(_emit_externs);
  assert(strstr(ir, "match_tok") != NULL);
  assert(strstr(ir, "declare") != NULL);
  free(ir);
}

// --- peg_ir_memo_get / peg_ir_memo_set ---

static IrWriter* _w_for_memo;

static void _emit_memo_ops(IrWriter* w) {
  const char* arg_types[] = {"ptr", "i32"};
  const char* arg_names[] = {"table", "col"};
  irwriter_define_start(w, "test_memo", "i32", 2, arg_types, arg_names);
  irwriter_bb(w);

  IrVal v = peg_ir_memo_get(w, "%Col.main", "%table", "%col", 0, 0);
  peg_ir_memo_set(w, "%Col.main", "%table", "%col", 0, 0, v);

  irwriter_ret(w, "i32", v);
  irwriter_define_end(w);
  (void)_w_for_memo;
}

TEST(test_memo_ops) {
  char* ir = _capture_ir(_emit_memo_ops);
  assert(strstr(ir, "getelementptr %Col.main") != NULL);
  assert(strstr(ir, "load i32") != NULL);
  assert(strstr(ir, "store i32") != NULL);
  free(ir);
}

// --- peg_ir_bit_test / peg_ir_bit_deny / peg_ir_bit_exclude ---

static void _emit_bit_ops(IrWriter* w) {
  const char* arg_types[] = {"ptr", "i32"};
  const char* arg_names[] = {"table", "col"};
  irwriter_define_start(w, "test_bits", "i32", 2, arg_types, arg_names);
  irwriter_bb(w);

  IrVal tested = peg_ir_bit_test(w, "%Col.main", "%table", "%col", 0, 1);
  peg_ir_bit_deny(w, "%Col.main", "%table", "%col", 0, 2);
  peg_ir_bit_exclude(w, "%Col.main", "%table", "%col", 0, 4);

  irwriter_ret(w, "i1", tested);
  irwriter_define_end(w);
}

TEST(test_bit_ops) {
  char* ir = _capture_ir(_emit_bit_ops);
  assert(strstr(ir, "getelementptr %Col.main") != NULL);
  assert(strstr(ir, "and i32") != NULL);
  assert(strstr(ir, "icmp ne") != NULL);
  free(ir);
}

// --- peg_ir_gen_rule_body (simple leaf) ---

static void _emit_rule_body_leaf(IrWriter* w) {
  peg_ir_emit_bt_defs(w);
  peg_ir_declare_externs(w);

  const char* arg_types[] = {"ptr", "i32"};
  const char* arg_names[] = {"table", "col"};
  irwriter_define_start(w, "parse_test", "i32", 2, arg_types, arg_names);
  irwriter_bb(w);

  int32_t fail_label = irwriter_label(w);

  PegUnit seq = {0};
  seq.kind = PEG_SEQ;
  seq.children = darray_new(sizeof(PegUnit), 0);

  PegUnit tok = {.kind = PEG_TOK, .name = strdup("NUM")};
  darray_push(seq.children, tok);

  Symtab tokens = {0};
  IrVal result = peg_ir_gen_rule_body(w, &seq, &tokens, "%Col.test", 0, fail_label);
  irwriter_ret(w, "i32", result);

  irwriter_bb_at(w, fail_label);
  irwriter_ret(w, "i32", irwriter_imm(w, "-1"));
  irwriter_define_end(w);

  symtab_free(&tokens);
  free(seq.children[0].name);
  darray_del(seq.children);
}

TEST(test_gen_rule_body_leaf) {
  char* ir = _capture_ir(_emit_rule_body_leaf);
  assert(strstr(ir, "define i32 @parse_test") != NULL);
  assert(strstr(ir, "match_tok") != NULL);
  assert(strstr(ir, "%BtStack") != NULL);
  free(ir);
}

// --- peg_ir_gen_rule_body (with branches) ---

static void _emit_rule_body_branches(IrWriter* w) {
  peg_ir_emit_bt_defs(w);
  peg_ir_declare_externs(w);

  const char* arg_types[] = {"ptr", "i32"};
  const char* arg_names[] = {"table", "col"};
  irwriter_define_start(w, "parse_branched", "i32", 2, arg_types, arg_names);
  irwriter_bb(w);

  int32_t fail_label = irwriter_label(w);

  PegUnit seq = {0};
  seq.kind = PEG_SEQ;
  seq.children = darray_new(sizeof(PegUnit), 0);

  PegUnit branches = {.kind = PEG_BRANCHES};
  branches.children = darray_new(sizeof(PegUnit), 0);

  PegUnit branch1 = {.kind = PEG_SEQ};
  branch1.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t1 = {.kind = PEG_TOK, .name = strdup("ID")};
  darray_push(branch1.children, t1);
  darray_push(branches.children, branch1);

  PegUnit branch2 = {.kind = PEG_SEQ};
  branch2.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t2 = {.kind = PEG_TOK, .name = strdup("NUM")};
  darray_push(branch2.children, t2);
  darray_push(branches.children, branch2);

  darray_push(seq.children, branches);

  Symtab tokens = {0};
  IrVal result = peg_ir_gen_rule_body(w, &seq, &tokens, "%Col.test", 1, fail_label);
  irwriter_ret(w, "i32", result);

  irwriter_bb_at(w, fail_label);
  irwriter_ret(w, "i32", irwriter_imm(w, "-1"));
  irwriter_define_end(w);

  symtab_free(&tokens);
  free(seq.children[0].children[0].children[0].name);
  darray_del(seq.children[0].children[0].children);
  free(seq.children[0].children[1].children[0].name);
  darray_del(seq.children[0].children[1].children);
  darray_del(seq.children[0].children);
  darray_del(seq.children);
}

TEST(test_gen_rule_body_branches) {
  char* ir = _capture_ir(_emit_rule_body_branches);
  assert(strstr(ir, "define i32 @parse_branched") != NULL);
  assert(strstr(ir, "shl i32") != NULL);
  free(ir);
}

// --- compile check: generated IR should be valid LLVM ---

TEST(test_peg_ir_compile) {
  FILE* f = fopen(BUILD_DIR "/test_peg_ir_out.ll", "w");
  assert(f);
  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "test_peg_ir.c", ".");

  peg_ir_emit_bt_defs(w);
  peg_ir_declare_externs(w);

  const char* arg_types[] = {"ptr", "i32"};
  const char* arg_names[] = {"table", "col"};
  irwriter_define_start(w, "parse_simple", "i32", 2, arg_types, arg_names);
  irwriter_bb(w);

  int32_t fail = irwriter_label(w);

  PegUnit seq = {0};
  seq.kind = PEG_SEQ;
  seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit tok = {.kind = PEG_TOK, .name = strdup("X")};
  darray_push(seq.children, tok);

  Symtab tokens = {0};
  IrVal r = peg_ir_gen_rule_body(w, &seq, &tokens, "%Col.s", 0, fail);
  irwriter_ret(w, "i32", r);

  irwriter_bb_at(w, fail);
  irwriter_ret(w, "i32", irwriter_imm(w, "-1"));
  irwriter_define_end(w);

  irwriter_end(w);
  irwriter_del(w);
  fclose(f);

  const char* null_out = compat_devnull_path();
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "%s -c %s -o %s 2>&1", compat_llvm_cc(), BUILD_DIR "/test_peg_ir_out.ll", null_out);
  int ret = system(cmd);
  assert(ret == 0);

  symtab_free(&tokens);
  free(seq.children[0].name);
  darray_del(seq.children);
}

int main(void) {
  printf("test_peg_ir:\n");

  RUN(test_bt_defs);
  RUN(test_declare_externs);
  RUN(test_memo_ops);
  RUN(test_bit_ops);
  RUN(test_gen_rule_body_leaf);
  RUN(test_gen_rule_body_branches);
  RUN(test_peg_ir_compile);

  printf("all ok\n");
  return 0;
}
