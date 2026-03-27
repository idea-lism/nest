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

#define TARGET "arm64-apple-macosx14.0.0"

// Helper: _capture irwriter output into a malloc'd string
static char* _capture(void (*fn)(IrWriter*)) {
  char* buf = NULL;
  size_t sz = 0;
  FILE* f = compat_open_memstream(&buf, &sz);
  assert(f);
  IrWriter* w = irwriter_new(f, TARGET);
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
  assert(strstr(out, "target triple = \"arm64-apple-macosx14.0.0\""));
  free(out);
}

static void _emit_simple_function(IrWriter* w) {
  irwriter_start(w, "test.ll", ".");

  const char* arg_types[] = {"i32", "i32"};
  const char* arg_names[] = {"state", "cp"};
  irwriter_define_start(w, "match", "{i32, i32}", 2, arg_types, arg_names);

  irwriter_bb(w, "entry");
  irwriter_ret(w, "{i32, i32}", "undef");

  irwriter_define_end(w);
  irwriter_end(w);
}

TEST(test_simple_function) {
  char* out = _capture(_emit_simple_function);
  assert(strstr(out, "define {i64, i64} @match(i64 %state_i64, i64 %cp_i64)"));
  assert(strstr(out, "%state = trunc i64 %state_i64 to i32"));
  assert(strstr(out, "%cp = trunc i64 %cp_i64 to i32"));
  assert(strstr(out, "entry:"));
  assert(strstr(out, "ret {i64, i64}"));
  assert(strstr(out, "}"));
  free(out);
}

static void _emit_binop(IrWriter* w) {
  irwriter_start(w, "test.ll", ".");
  const char* arg_types[] = {"i32"};
  const char* arg_names[] = {"x"};
  irwriter_define_start(w, "f", "i32", 1, arg_types, arg_names);
  irwriter_bb(w, "entry");

  int32_t r1 = irwriter_binop_imm(w, "add", "i32", "%x", 1);
  assert(r1 == 0);

  char r1s[16];
  snprintf(r1s, sizeof(r1s), "%%r%d", r1);
  int32_t r2 = irwriter_binop(w, "mul", "i32", "%x", r1s);
  assert(r2 == 1);

  irwriter_ret(w, "i32", "%r1");
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

  irwriter_bb(w, "entry");
  char cmp_name[16];
  snprintf(cmp_name, sizeof(cmp_name), "%%r%d", irwriter_icmp_imm(w, "sge", "i32", "%x", 0));
  irwriter_br_cond(w, cmp_name, "positive", "negative");

  irwriter_bb(w, "positive");
  irwriter_ret(w, "i32", "%x");

  irwriter_bb(w, "negative");
  irwriter_binop_imm(w, "sub", "i32", "0", 0);
  irwriter_ret(w, "i32", "%r1");

  irwriter_define_end(w);
  irwriter_end(w);
}

TEST(test_icmp_branch) {
  char* out = _capture(_emit_icmp_branch);
  assert(strstr(out, "%r0 = icmp sge i32 %x, 0"));
  assert(strstr(out, "br i1 %r0, label %positive, label %negative"));
  assert(strstr(out, "positive:"));
  assert(strstr(out, "negative:"));
  free(out);
}

static void _emit_switch(IrWriter* w) {
  irwriter_start(w, "test.ll", ".");
  const char* arg_types[] = {"i32"};
  const char* arg_names[] = {"s"};
  irwriter_define_start(w, "dispatch", "void", 1, arg_types, arg_names);

  irwriter_bb(w, "entry");
  irwriter_switch_start(w, "i32", "%s", "dead");
  irwriter_switch_case(w, "i32", 0, "state0");
  irwriter_switch_case(w, "i32", 1, "state1");
  irwriter_switch_end(w);

  irwriter_bb(w, "state0");
  irwriter_br(w, "done");

  irwriter_bb(w, "state1");
  irwriter_br(w, "done");

  irwriter_bb(w, "dead");
  irwriter_br(w, "done");

  irwriter_bb(w, "done");
  irwriter_ret(w, "void", "");

  irwriter_define_end(w);
  irwriter_end(w);
}

TEST(test_switch) {
  char* out = _capture(_emit_switch);
  assert(strstr(out, "switch i32 %s, label %dead ["));
  assert(strstr(out, "i32 0, label %state0"));
  assert(strstr(out, "i32 1, label %state1"));
  assert(strstr(out, "]"));
  free(out);
}

static void _emit_insertvalue(IrWriter* w) {
  irwriter_start(w, "test.ll", ".");
  const char* arg_types[] = {"i32", "i32"};
  const char* arg_names[] = {"state", "cp"};
  irwriter_define_start(w, "match", "{i32, i32}", 2, arg_types, arg_names);

  irwriter_bb(w, "entry");
  char r0_name[16];
  snprintf(r0_name, sizeof(r0_name), "%%r%d", irwriter_insertvalue_imm(w, "{i32, i32}", "undef", "i32", 1, 0));
  char r1_name[16];
  snprintf(r1_name, sizeof(r1_name), "%%r%d", irwriter_insertvalue_imm(w, "{i32, i32}", r0_name, "i32", 0, 1));
  irwriter_ret(w, "{i32, i32}", r1_name);

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

  irwriter_bb(w, "entry");
  irwriter_dbg(w, 10, 5);
  irwriter_binop_imm(w, "add", "i32", "%x", 1);
  irwriter_dbg(w, 11, 3);
  irwriter_ret(w, "i32", "%r0");

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

// Full DFA-style function: mimics what aut_gen_dfa would produce
static void _emit_dfa_function(IrWriter* w) {
  irwriter_start(w, "dfa.rules", ".");

  const char* arg_types[] = {"i32", "i32"};
  const char* arg_names[] = {"state", "cp"};
  irwriter_define_start(w, "match", "{i32, i32}", 2, arg_types, arg_names);

  // entry: switch on state
  irwriter_bb(w, "entry");
  irwriter_dbg(w, 1, 1);
  irwriter_switch_start(w, "i32", "%state", "dead");
  irwriter_switch_case(w, "i32", 0, "state0");
  irwriter_switch_end(w);

  // state0: check if cp in [0x41, 0x5A]
  irwriter_bb(w, "state0");
  irwriter_dbg(w, 2, 1);
  char lo_name[16];
  snprintf(lo_name, sizeof(lo_name), "%%r%d", irwriter_icmp_imm(w, "sge", "i32", "%cp", 0x41));
  char hi_name[16];
  snprintf(hi_name, sizeof(hi_name), "%%r%d", irwriter_icmp_imm(w, "sle", "i32", "%cp", 0x5A));
  char in_range_name[16];
  snprintf(in_range_name, sizeof(in_range_name), "%%r%d", irwriter_binop(w, "and", "i1", lo_name, hi_name));
  irwriter_br_cond(w, in_range_name, "s0_match", "s0_fail");

  // s0_match: return {1, 0}
  irwriter_bb(w, "s0_match");
  irwriter_dbg(w, 2, 5);
  char v0_name[16];
  snprintf(v0_name, sizeof(v0_name), "%%r%d", irwriter_insertvalue_imm(w, "{i32, i32}", "undef", "i32", 1, 0));
  char v1_name[16];
  snprintf(v1_name, sizeof(v1_name), "%%r%d", irwriter_insertvalue_imm(w, "{i32, i32}", v0_name, "i32", 0, 1));
  irwriter_ret(w, "{i32, i32}", v1_name);

  // s0_fail: return {0, -2}
  irwriter_bb(w, "s0_fail");
  irwriter_dbg(w, 2, 10);
  char v2_name[16];
  snprintf(v2_name, sizeof(v2_name), "%%r%d", irwriter_insertvalue_imm(w, "{i32, i32}", "undef", "i32", 0, 0));
  char v3_name[16];
  snprintf(v3_name, sizeof(v3_name), "%%r%d", irwriter_insertvalue_imm(w, "{i32, i32}", v2_name, "i32", -2, 1));
  irwriter_ret(w, "{i32, i32}", v3_name);

  // dead: return {0, -2}
  irwriter_bb(w, "dead");
  irwriter_dbg(w, 1, 1);
  char v4_name[16];
  snprintf(v4_name, sizeof(v4_name), "%%r%d", irwriter_insertvalue_imm(w, "{i32, i32}", "undef", "i32", 0, 0));
  char v5_name[16];
  snprintf(v5_name, sizeof(v5_name), "%%r%d", irwriter_insertvalue_imm(w, "{i32, i32}", v4_name, "i32", -2, 1));
  irwriter_ret(w, "{i32, i32}", v5_name);

  irwriter_define_end(w);
  irwriter_end(w);
}

TEST(test_dfa_function) {
  char* out = _capture(_emit_dfa_function);
  assert(strstr(out, "define {i64, i64} @match(i64 %state_i64, i64 %cp_i64)"));
  assert(strstr(out, "switch i32 %state, label %dead"));
  assert(strstr(out, "state0:"));
  assert(strstr(out, "s0_match:"));
  assert(strstr(out, "s0_fail:"));
  assert(strstr(out, "dead:"));
  assert(strstr(out, "insertvalue {i32, i32}"));
  assert(strstr(out, "ret {i64, i64}"));
  assert(strstr(out, "DISubprogram(name: \"match\""));
  assert(strstr(out, "DILocation"));
  free(out);
}

TEST(test_lifecycle) {
  FILE* f = compat_devnull_w();
  assert(f);
  IrWriter* w = irwriter_new(f, TARGET);
  assert(w);

  irwriter_start(w, "test.ll", ".");
  const char* arg_types[] = {"i32"};
  const char* arg_names[] = {"x"};
  irwriter_define_start(w, "f", "i32", 1, arg_types, arg_names);
  irwriter_bb(w, "entry");
  irwriter_ret(w, "i32", "%x");
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
  IrWriter* w = irwriter_new(f, TARGET);

  irwriter_start(w, "dfa.rules", ".");

  const char* arg_types[] = {"i32", "i32"};
  const char* arg_names[] = {"state", "cp"};
  irwriter_define_start(w, "match", "{i32, i32}", 2, arg_types, arg_names);

  irwriter_bb(w, "entry");
  irwriter_dbg(w, 1, 1);
  irwriter_switch_start(w, "i32", "%state", "dead");
  irwriter_switch_case(w, "i32", 0, "state0");
  irwriter_switch_end(w);

  irwriter_bb(w, "state0");
  irwriter_dbg(w, 2, 1);
  char lo_n[16];
  snprintf(lo_n, sizeof(lo_n), "%%r%d", irwriter_icmp_imm(w, "sge", "i32", "%cp", 0x41));
  char hi_n[16];
  snprintf(hi_n, sizeof(hi_n), "%%r%d", irwriter_icmp_imm(w, "sle", "i32", "%cp", 0x5A));
  char inr_n[16];
  snprintf(inr_n, sizeof(inr_n), "%%r%d", irwriter_binop(w, "and", "i1", lo_n, hi_n));
  irwriter_br_cond(w, inr_n, "s0_match", "s0_fail");

  irwriter_bb(w, "s0_match");
  irwriter_dbg(w, 2, 5);
  char v0n[16];
  snprintf(v0n, sizeof(v0n), "%%r%d", irwriter_insertvalue_imm(w, "{i32, i32}", "undef", "i32", 1, 0));
  char v1n[16];
  snprintf(v1n, sizeof(v1n), "%%r%d", irwriter_insertvalue_imm(w, "{i32, i32}", v0n, "i32", 0, 1));
  irwriter_ret(w, "{i32, i32}", v1n);

  irwriter_bb(w, "s0_fail");
  irwriter_dbg(w, 2, 10);
  char v2n[16];
  snprintf(v2n, sizeof(v2n), "%%r%d", irwriter_insertvalue_imm(w, "{i32, i32}", "undef", "i32", 0, 0));
  char v3n[16];
  snprintf(v3n, sizeof(v3n), "%%r%d", irwriter_insertvalue_imm(w, "{i32, i32}", v2n, "i32", -2, 1));
  irwriter_ret(w, "{i32, i32}", v3n);

  irwriter_bb(w, "dead");
  irwriter_dbg(w, 1, 1);
  char v4n[16];
  snprintf(v4n, sizeof(v4n), "%%r%d", irwriter_insertvalue_imm(w, "{i32, i32}", "undef", "i32", 0, 0));
  char v5n[16];
  snprintf(v5n, sizeof(v5n), "%%r%d", irwriter_insertvalue_imm(w, "{i32, i32}", v4n, "i32", -2, 1));
  irwriter_ret(w, "{i32, i32}", v5n);

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
