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

static char* _capture(void (*fn)(IrWriter*)) {
  char* buf = NULL;
  size_t sz = 0;
  FILE* f = compat_open_memstream(&buf, &sz);
  assert(f);
  IrWriter* w = irwriter_new(f, NULL);
  fn(w);
  irwriter_del(w);
  compat_close_memstream(f, &buf, &sz);
  return buf;
}

// --- Tests ---

static void _emit_module_prelude(IrWriter* w) { irwriter_start(w, "test.ll", "."); }

TEST(test_module_prelude) {
  char* out = _capture(_emit_module_prelude);
  assert(strstr(out, "source_filename = \"test.ll\""));
  assert(!strstr(out, "target triple"));
  free(out);
}

static void _emit_simple_function(IrWriter* w) {
  irwriter_start(w, "test.ll", ".");

  const char* arg_types[] = {"i32", "i32"};
  const char* arg_names[] = {"state", "cp"};
  irwriter_define_start(w, "match", "{i32, i32}", 2, arg_types, arg_names);

  irwriter_bb(w); // L0
  IrVal zero = irwriter_imm(w, "0");
  IrVal undef_r = irwriter_insertvalue(w, "{i32, i32}", -1, "i32", zero, 0);
  irwriter_ret(w, "{i32, i32}", undef_r);

  irwriter_define_end(w);
  irwriter_end(w);
}

TEST(test_simple_function) {
  char* out = _capture(_emit_simple_function);
  assert(strstr(out, "define {i64, i64} @match(i64 %state_i64, i64 %cp_i64)"));
  assert(strstr(out, "%state = trunc i64 %state_i64 to i32"));
  assert(strstr(out, "%cp = trunc i64 %cp_i64 to i32"));
  assert(strstr(out, "L0:"));
  assert(strstr(out, "insertvalue {i32, i32} undef, i32 0, 0"));
  assert(strstr(out, "ret {i64, i64}"));
  assert(strstr(out, "}"));
  free(out);
}

static void _emit_binop(IrWriter* w) {
  irwriter_start(w, "test.ll", ".");
  const char* arg_types[] = {"i32"};
  const char* arg_names[] = {"x"};
  irwriter_define_start(w, "f", "i32", 1, arg_types, arg_names);
  irwriter_bb(w); // L0

  IrVal x = irwriter_imm(w, "%x");
  IrVal one = irwriter_imm(w, "1");
  IrVal r0 = irwriter_binop(w, "add", "i32", x, one);
  assert(r0 == 0);

  IrVal r1 = irwriter_binop(w, "mul", "i32", x, r0);
  assert(r1 == 1);

  irwriter_ret(w, "i32", r1);
  irwriter_define_end(w);
  irwriter_end(w);
}

TEST(test_binop) {
  char* out = _capture(_emit_binop);
  assert(strstr(out, "%r0 = add i32 %x, 1"));
  assert(strstr(out, "%r1 = mul i32 %x, %r0"));
  assert(strstr(out, "ret i32 %r1"));
  free(out);
}

static void _emit_icmp_branch(IrWriter* w) {
  irwriter_start(w, "test.ll", ".");
  const char* arg_types[] = {"i32"};
  const char* arg_names[] = {"x"};
  irwriter_define_start(w, "f", "i32", 1, arg_types, arg_names);

  int32_t positive = irwriter_label(w); // L0
  int32_t negative = irwriter_label(w); // L1

  irwriter_bb(w); // entry, emits prologue
  IrVal x = irwriter_imm(w, "%x");
  IrVal cmp = irwriter_icmp(w, "sge", "i32", x, irwriter_imm(w, "0"));
  irwriter_br_cond(w, cmp, positive, negative);

  irwriter_bb_at(w, positive);
  irwriter_ret(w, "i32", x);

  irwriter_bb_at(w, negative);
  irwriter_ret(w, "i32", irwriter_imm(w, "0"));

  irwriter_define_end(w);
  irwriter_end(w);
}

TEST(test_icmp_branch) {
  char* out = _capture(_emit_icmp_branch);
  assert(strstr(out, "%r0 = icmp sge i32 %x, 0"));
  assert(strstr(out, "br i1 %r0, label %L0, label %L1"));
  assert(strstr(out, "L0:"));
  assert(strstr(out, "ret i32 %x"));
  assert(strstr(out, "L1:"));
  assert(strstr(out, "ret i32 0"));
  free(out);
}

static void _emit_switch(IrWriter* w) {
  irwriter_start(w, "test.ll", ".");
  const char* arg_types[] = {"i32"};
  const char* arg_names[] = {"s"};
  irwriter_define_start(w, "dispatch", "void", 1, arg_types, arg_names);

  int32_t dead = irwriter_label(w);   // L0
  int32_t state0 = irwriter_label(w); // L1
  int32_t state1 = irwriter_label(w); // L2
  int32_t done = irwriter_label(w);   // L3

  irwriter_bb(w); // entry
  irwriter_switch_start(w, "i32", irwriter_imm(w, "%s"), dead);
  irwriter_switch_case(w, "i32", 0, state0);
  irwriter_switch_case(w, "i32", 1, state1);
  irwriter_switch_end(w);

  irwriter_bb_at(w, state0);
  irwriter_br(w, done);

  irwriter_bb_at(w, state1);
  irwriter_br(w, done);

  irwriter_bb_at(w, dead);
  irwriter_br(w, done);

  irwriter_bb_at(w, done);
  irwriter_ret_void(w);

  irwriter_define_end(w);
  irwriter_end(w);
}

TEST(test_switch) {
  char* out = _capture(_emit_switch);
  assert(strstr(out, "switch i32 %s, label %L0 ["));
  assert(strstr(out, "i32 0, label %L1"));
  assert(strstr(out, "i32 1, label %L2"));
  assert(strstr(out, "]"));
  free(out);
}

static void _emit_insertvalue(IrWriter* w) {
  irwriter_start(w, "test.ll", ".");
  const char* arg_types[] = {"i32", "i32"};
  const char* arg_names[] = {"state", "cp"};
  irwriter_define_start(w, "match", "{i32, i32}", 2, arg_types, arg_names);

  irwriter_bb(w); // L0
  IrVal r0 = irwriter_insertvalue(w, "{i32, i32}", -1, "i32", irwriter_imm(w, "1"), 0);
  IrVal r1 = irwriter_insertvalue(w, "{i32, i32}", r0, "i32", irwriter_imm(w, "0"), 1);
  irwriter_ret(w, "{i32, i32}", r1);

  irwriter_define_end(w);
  irwriter_end(w);
}

TEST(test_insertvalue) {
  char* out = _capture(_emit_insertvalue);
  assert(strstr(out, "%r0 = insertvalue {i32, i32} undef, i32 1, 0"));
  assert(strstr(out, "%r1 = insertvalue {i32, i32} %r0, i32 0, 1"));
  assert(strstr(out, "ret {i64, i64}"));
  free(out);
}

static void _emit_debug_locations(IrWriter* w) {
  irwriter_start(w, "test.ll", ".");
  const char* arg_types[] = {"i32"};
  const char* arg_names[] = {"x"};
  irwriter_define_start(w, "f", "i32", 1, arg_types, arg_names);

  irwriter_bb(w); // L0
  irwriter_dbg(w, 10, 5);
  IrVal x = irwriter_imm(w, "%x");
  IrVal one = irwriter_imm(w, "1");
  IrVal dbg_r = irwriter_binop(w, "add", "i32", x, one);
  irwriter_dbg(w, 11, 3);
  irwriter_ret(w, "i32", dbg_r);

  irwriter_define_end(w);
  irwriter_end(w);
}

TEST(test_debug_locations) {
  char* out = _capture(_emit_debug_locations);
  assert(strstr(out, "!dbg !"));
  assert(strstr(out, "DILocation(line: 10, column: 5"));
  assert(strstr(out, "DILocation(line: 11, column: 3"));
  assert(strstr(out, "DICompileUnit"));
  assert(strstr(out, "DISubprogram"));
  assert(strstr(out, "!llvm.dbg.cu"));
  assert(strstr(out, "DIFile(filename: \"test.ll\", directory: \".\")"));
  free(out);
}

static void _emit_dfa_function(IrWriter* w) {
  irwriter_start(w, "dfa.rules", ".");

  const char* arg_types[] = {"i32", "i32"};
  const char* arg_names[] = {"state", "cp"};
  irwriter_define_start(w, "match", "{i32, i32}", 2, arg_types, arg_names);

  int32_t dead = irwriter_label(w);     // L0
  int32_t state0 = irwriter_label(w);   // L1
  int32_t s0_match = irwriter_label(w); // L2
  int32_t s0_fail = irwriter_label(w);  // L3

  // entry: switch on state
  irwriter_bb(w);
  irwriter_dbg(w, 1, 1);
  irwriter_switch_start(w, "i32", irwriter_imm(w, "%state"), dead);
  irwriter_switch_case(w, "i32", 0, state0);
  irwriter_switch_end(w);

  // state0: check if cp in [0x41, 0x5A]
  irwriter_bb_at(w, state0);
  irwriter_dbg(w, 2, 1);
  IrVal cp = irwriter_imm(w, "%cp");
  IrVal lo = irwriter_icmp(w, "sge", "i32", cp, irwriter_imm(w, "65"));
  IrVal hi = irwriter_icmp(w, "sle", "i32", cp, irwriter_imm(w, "90"));
  IrVal in_range = irwriter_binop(w, "and", "i1", lo, hi);
  irwriter_br_cond(w, in_range, s0_match, s0_fail);

  // s0_match: return {1, 0}
  irwriter_bb_at(w, s0_match);
  irwriter_dbg(w, 2, 5);
  IrVal v0_reg = irwriter_insertvalue(w, "{i32, i32}", -1, "i32", irwriter_imm(w, "1"), 0);
  IrVal v1_reg = irwriter_insertvalue(w, "{i32, i32}", v0_reg, "i32", irwriter_imm(w, "0"), 1);
  irwriter_ret(w, "{i32, i32}", v1_reg);

  // s0_fail: return {0, -2}
  irwriter_bb_at(w, s0_fail);
  irwriter_dbg(w, 2, 10);
  IrVal v2_reg = irwriter_insertvalue(w, "{i32, i32}", -1, "i32", irwriter_imm(w, "0"), 0);
  IrVal v3_reg = irwriter_insertvalue(w, "{i32, i32}", v2_reg, "i32", irwriter_imm(w, "-2"), 1);
  irwriter_ret(w, "{i32, i32}", v3_reg);

  // dead: return {0, -2}
  irwriter_bb_at(w, dead);
  irwriter_dbg(w, 1, 1);
  IrVal v4_reg = irwriter_insertvalue(w, "{i32, i32}", -1, "i32", irwriter_imm(w, "0"), 0);
  IrVal v5_reg = irwriter_insertvalue(w, "{i32, i32}", v4_reg, "i32", irwriter_imm(w, "-2"), 1);
  irwriter_ret(w, "{i32, i32}", v5_reg);

  irwriter_define_end(w);
  irwriter_end(w);
}

TEST(test_dfa_function) {
  char* out = _capture(_emit_dfa_function);
  assert(strstr(out, "define {i64, i64} @match(i64 %state_i64, i64 %cp_i64)"));
  assert(strstr(out, "switch i32 %state, label %L0"));
  assert(strstr(out, "i32 0, label %L1"));
  assert(strstr(out, "L1:"));
  assert(strstr(out, "L2:"));
  assert(strstr(out, "L3:"));
  assert(strstr(out, "L0:"));
  assert(strstr(out, "insertvalue {i32, i32}"));
  assert(strstr(out, "ret {i64, i64}"));
  assert(strstr(out, "DISubprogram(name: \"match\""));
  assert(strstr(out, "DILocation"));
  free(out);
}

TEST(test_lifecycle) {
  FILE* f = compat_devnull_w();
  assert(f);
  IrWriter* w = irwriter_new(f, NULL);
  assert(w);

  irwriter_start(w, "test.ll", ".");
  const char* arg_types[] = {"i32"};
  const char* arg_names[] = {"x"};
  irwriter_define_start(w, "f", "i32", 1, arg_types, arg_names);
  irwriter_bb(w);
  irwriter_ret(w, "i32", irwriter_imm(w, "%x"));
  irwriter_define_end(w);
  irwriter_end(w);

  irwriter_del(w);
  fclose(f);
}

TEST(test_clang_compile) {
  char ll_path[128], obj_path[128];
  snprintf(ll_path, sizeof(ll_path), "%s/test_irwriter.ll", BUILD_DIR);
  snprintf(obj_path, sizeof(obj_path), "%s/test_irwriter.o", BUILD_DIR);
  FILE* f = fopen(ll_path, "w");
  assert(f);
  IrWriter* w = irwriter_new(f, NULL);

  irwriter_start(w, "dfa.rules", ".");

  const char* arg_types[] = {"i32", "i32"};
  const char* arg_names[] = {"state", "cp"};
  irwriter_define_start(w, "match", "{i32, i32}", 2, arg_types, arg_names);

  int32_t state0 = irwriter_label(w);   // L0
  int32_t dead = irwriter_label(w);     // L1
  int32_t s0_match = irwriter_label(w); // L2
  int32_t s0_fail = irwriter_label(w);  // L3

  // entry BB (emits prologue)
  irwriter_bb(w); // L4
  irwriter_dbg(w, 1, 1);
  irwriter_switch_start(w, "i32", irwriter_imm(w, "%state"), dead);
  irwriter_switch_case(w, "i32", 0, state0);
  irwriter_switch_end(w);

  // state0
  irwriter_bb_at(w, state0);
  irwriter_dbg(w, 2, 1);
  IrVal cp = irwriter_imm(w, "%cp");
  IrVal lo_r = irwriter_icmp(w, "sge", "i32", cp, irwriter_imm(w, "65"));
  IrVal hi_r = irwriter_icmp(w, "sle", "i32", cp, irwriter_imm(w, "90"));
  IrVal in_range = irwriter_binop(w, "and", "i1", lo_r, hi_r);
  irwriter_br_cond(w, in_range, s0_match, s0_fail);

  // s0_match
  irwriter_bb_at(w, s0_match);
  irwriter_dbg(w, 2, 5);
  IrVal v0r = irwriter_insertvalue(w, "{i32, i32}", -1, "i32", irwriter_imm(w, "1"), 0);
  IrVal v1r = irwriter_insertvalue(w, "{i32, i32}", v0r, "i32", irwriter_imm(w, "0"), 1);
  irwriter_ret(w, "{i32, i32}", v1r);

  // s0_fail
  irwriter_bb_at(w, s0_fail);
  irwriter_dbg(w, 2, 10);
  IrVal v2r = irwriter_insertvalue(w, "{i32, i32}", -1, "i32", irwriter_imm(w, "0"), 0);
  IrVal v3r = irwriter_insertvalue(w, "{i32, i32}", v2r, "i32", irwriter_imm(w, "-2"), 1);
  irwriter_ret(w, "{i32, i32}", v3r);

  // dead
  irwriter_bb_at(w, dead);
  irwriter_dbg(w, 1, 1);
  IrVal v4r = irwriter_insertvalue(w, "{i32, i32}", -1, "i32", irwriter_imm(w, "0"), 0);
  IrVal v5r = irwriter_insertvalue(w, "{i32, i32}", v4r, "i32", irwriter_imm(w, "-2"), 1);
  irwriter_ret(w, "{i32, i32}", v5r);

  irwriter_define_end(w);
  irwriter_end(w);
  irwriter_del(w);
  fclose(f);

  char cmd[256];
  snprintf(cmd, sizeof(cmd), "%s -c %s -o %s 2>&1", compat_llvm_cc(), ll_path, obj_path);
  FILE* p = compat_popen(cmd, "r");
  assert(p);
  char output[4096] = {0};
  size_t n = fread(output, 1, sizeof(output) - 1, p);
  output[n] = '\0';
  int status = compat_pclose(p);
  if (status != 0) {
    fprintf(stderr, "\nclang failed:\n%s\n", output);
    FILE* ll = fopen(ll_path, "r");
    if (ll) {
      char line[512];
      while (fgets(line, sizeof(line), ll)) {
        fputs(line, stderr);
      }
      fclose(ll);
    }
  }
  assert(status == 0);
  remove(obj_path);
  remove(ll_path);
}

int main(void) {
  printf("test_irwriter:\n");
  RUN(test_module_prelude);
  RUN(test_simple_function);
  RUN(test_binop);
  RUN(test_icmp_branch);
  RUN(test_switch);
  RUN(test_insertvalue);
  RUN(test_debug_locations);
  RUN(test_dfa_function);
  RUN(test_lifecycle);
  RUN(test_clang_compile);
  printf("all ok\n");
  return 0;
}
