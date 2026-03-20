#include "../src/irwriter.h"
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

// Helper: capture irwriter output into a malloc'd string
static char* capture(void (*fn)(IrWriter*)) {
  char* buf = NULL;
  size_t sz = 0;
  FILE* f = open_memstream(&buf, &sz);
  assert(f);
  IrWriter* w = irwriter_new(f, TARGET);
  fn(w);
  irwriter_del(w);
  fclose(f);
  return buf;
}

// --- Tests ---

static void emit_module_prelude(IrWriter* w) { irwriter_start(w, "test.ll", "."); }

TEST(test_module_prelude) {
  char* out = capture(emit_module_prelude);
  assert(strstr(out, "source_filename = \"test.ll\""));
  assert(strstr(out, "target triple = \"arm64-apple-macosx14.0.0\""));
  free(out);
}

static void emit_simple_function(IrWriter* w) {
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
  char* out = capture(emit_simple_function);
  assert(strstr(out, "define {i32, i32} @match(i32 %state, i32 %cp)"));
  assert(strstr(out, "entry:"));
  assert(strstr(out, "ret {i32, i32} undef"));
  assert(strstr(out, "}"));
  free(out);
}

static void emit_binop(IrWriter* w) {
  irwriter_start(w, "test.ll", ".");
  const char* arg_types[] = {"i32"};
  const char* arg_names[] = {"x"};
  irwriter_define_start(w, "f", "i32", 1, arg_types, arg_names);
  irwriter_bb(w, "entry");

  const char* r1 = irwriter_binop_imm(w, "add", "i32", "%x", 1);
  assert(strcmp(r1, "%r0") == 0);

  const char* r2 = irwriter_binop(w, "mul", "i32", "%x", r1);
  // r1 is overwritten by r2 since they share the buf, but the fprintf already happened
  assert(strcmp(r2, "%r1") == 0);

  irwriter_ret(w, "i32", "%r1");
  irwriter_define_end(w);
  irwriter_end(w);
}

TEST(test_binop) {
  char* out = capture(emit_binop);
  assert(strstr(out, "%r0 = add i32 %x, 1"));
  assert(strstr(out, "%r1 = mul i32 %x, %r0"));
  assert(strstr(out, "ret i32 %r1"));
  free(out);
}

static void emit_icmp_branch(IrWriter* w) {
  irwriter_start(w, "test.ll", ".");
  const char* arg_types[] = {"i32"};
  const char* arg_names[] = {"x"};
  irwriter_define_start(w, "f", "i32", 1, arg_types, arg_names);

  irwriter_bb(w, "entry");
  const char* cmp = irwriter_icmp_imm(w, "sge", "i32", "%x", 0);
  // Need to save since next call overwrites buf
  char cmp_name[32];
  snprintf(cmp_name, sizeof(cmp_name), "%s", cmp);
  irwriter_br_cond(w, cmp_name, "positive", "negative");

  irwriter_bb(w, "positive");
  irwriter_ret(w, "i32", "%x");

  irwriter_bb(w, "negative");
  const char* neg = irwriter_binop_imm(w, "sub", "i32", "0", 0);
  (void)neg;
  irwriter_ret(w, "i32", "%r1");

  irwriter_define_end(w);
  irwriter_end(w);
}

TEST(test_icmp_branch) {
  char* out = capture(emit_icmp_branch);
  assert(strstr(out, "%r0 = icmp sge i32 %x, 0"));
  assert(strstr(out, "br i1 %r0, label %positive, label %negative"));
  assert(strstr(out, "positive:"));
  assert(strstr(out, "negative:"));
  free(out);
}

static void emit_switch(IrWriter* w) {
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
  char* out = capture(emit_switch);
  assert(strstr(out, "switch i32 %s, label %dead ["));
  assert(strstr(out, "i32 0, label %state0"));
  assert(strstr(out, "i32 1, label %state1"));
  assert(strstr(out, "]"));
  free(out);
}

static void emit_insertvalue(IrWriter* w) {
  irwriter_start(w, "test.ll", ".");
  const char* arg_types[] = {"i32", "i32"};
  const char* arg_names[] = {"state", "cp"};
  irwriter_define_start(w, "match", "{i32, i32}", 2, arg_types, arg_names);

  irwriter_bb(w, "entry");
  const char* r0 = irwriter_insertvalue_imm(w, "{i32, i32}", "undef", "i32", 1, 0);
  char r0_name[32];
  snprintf(r0_name, sizeof(r0_name), "%s", r0);
  const char* r1 = irwriter_insertvalue_imm(w, "{i32, i32}", r0_name, "i32", 0, 1);
  char r1_name[32];
  snprintf(r1_name, sizeof(r1_name), "%s", r1);
  irwriter_ret(w, "{i32, i32}", r1_name);

  irwriter_define_end(w);
  irwriter_end(w);
}

TEST(test_insertvalue) {
  char* out = capture(emit_insertvalue);
  assert(strstr(out, "%r0 = insertvalue {i32, i32} undef, i32 1, 0"));
  assert(strstr(out, "%r1 = insertvalue {i32, i32} %r0, i32 0, 1"));
  assert(strstr(out, "ret {i32, i32} %r1"));
  free(out);
}

static void emit_debug_locations(IrWriter* w) {
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
  char* out = capture(emit_debug_locations);
  // Instructions should have !dbg references
  assert(strstr(out, "!dbg !"));
  // Metadata should contain DILocation
  assert(strstr(out, "DILocation(line: 10, column: 5"));
  assert(strstr(out, "DILocation(line: 11, column: 3"));
  // Module-level debug metadata
  assert(strstr(out, "DICompileUnit"));
  assert(strstr(out, "DISubprogram"));
  assert(strstr(out, "!llvm.dbg.cu"));
  assert(strstr(out, "DIFile(filename: \"test.ll\", directory: \".\")"));
  free(out);
}

// Full DFA-style function: mimics what aut_gen_dfa would produce
static void emit_dfa_function(IrWriter* w) {
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
  const char* lo = irwriter_icmp_imm(w, "sge", "i32", "%cp", 0x41);
  char lo_name[32];
  snprintf(lo_name, sizeof(lo_name), "%s", lo);
  const char* hi = irwriter_icmp_imm(w, "sle", "i32", "%cp", 0x5A);
  char hi_name[32];
  snprintf(hi_name, sizeof(hi_name), "%s", hi);
  const char* in_range = irwriter_binop(w, "and", "i1", lo_name, hi_name);
  char in_range_name[32];
  snprintf(in_range_name, sizeof(in_range_name), "%s", in_range);
  irwriter_br_cond(w, in_range_name, "s0_match", "s0_fail");

  // s0_match: return {1, 0}
  irwriter_bb(w, "s0_match");
  irwriter_dbg(w, 2, 5);
  const char* v0 = irwriter_insertvalue_imm(w, "{i32, i32}", "undef", "i32", 1, 0);
  char v0_name[32];
  snprintf(v0_name, sizeof(v0_name), "%s", v0);
  const char* v1 = irwriter_insertvalue_imm(w, "{i32, i32}", v0_name, "i32", 0, 1);
  char v1_name[32];
  snprintf(v1_name, sizeof(v1_name), "%s", v1);
  irwriter_ret(w, "{i32, i32}", v1_name);

  // s0_fail: return {0, -2}
  irwriter_bb(w, "s0_fail");
  irwriter_dbg(w, 2, 10);
  const char* v2 = irwriter_insertvalue_imm(w, "{i32, i32}", "undef", "i32", 0, 0);
  char v2_name[32];
  snprintf(v2_name, sizeof(v2_name), "%s", v2);
  const char* v3 = irwriter_insertvalue_imm(w, "{i32, i32}", v2_name, "i32", -2, 1);
  char v3_name[32];
  snprintf(v3_name, sizeof(v3_name), "%s", v3);
  irwriter_ret(w, "{i32, i32}", v3_name);

  // dead: return {0, -2}
  irwriter_bb(w, "dead");
  irwriter_dbg(w, 1, 1);
  const char* v4 = irwriter_insertvalue_imm(w, "{i32, i32}", "undef", "i32", 0, 0);
  char v4_name[32];
  snprintf(v4_name, sizeof(v4_name), "%s", v4);
  const char* v5 = irwriter_insertvalue_imm(w, "{i32, i32}", v4_name, "i32", -2, 1);
  char v5_name[32];
  snprintf(v5_name, sizeof(v5_name), "%s", v5);
  irwriter_ret(w, "{i32, i32}", v5_name);

  irwriter_define_end(w);
  irwriter_end(w);
}

TEST(test_dfa_function) {
  char* out = capture(emit_dfa_function);
  // Structural checks
  assert(strstr(out, "define {i32, i32} @match(i32 %state, i32 %cp)"));
  assert(strstr(out, "switch i32 %state, label %dead"));
  assert(strstr(out, "state0:"));
  assert(strstr(out, "s0_match:"));
  assert(strstr(out, "s0_fail:"));
  assert(strstr(out, "dead:"));
  assert(strstr(out, "insertvalue {i32, i32}"));
  assert(strstr(out, "ret {i32, i32}"));
  // Debug
  assert(strstr(out, "DISubprogram(name: \"match\""));
  assert(strstr(out, "DILocation"));
  free(out);
}

TEST(test_lifecycle) {
  FILE* f = fopen("/dev/null", "w");
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
  const char* ll_path = "/tmp/test_irwriter.ll";
  const char* obj_path = "/tmp/test_irwriter.o";
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
  const char* lo = irwriter_icmp_imm(w, "sge", "i32", "%cp", 0x41);
  char lo_n[32];
  snprintf(lo_n, 32, "%s", lo);
  const char* hi = irwriter_icmp_imm(w, "sle", "i32", "%cp", 0x5A);
  char hi_n[32];
  snprintf(hi_n, 32, "%s", hi);
  const char* inr = irwriter_binop(w, "and", "i1", lo_n, hi_n);
  char inr_n[32];
  snprintf(inr_n, 32, "%s", inr);
  irwriter_br_cond(w, inr_n, "s0_match", "s0_fail");

  irwriter_bb(w, "s0_match");
  irwriter_dbg(w, 2, 5);
  const char* v0 = irwriter_insertvalue_imm(w, "{i32, i32}", "undef", "i32", 1, 0);
  char v0n[32];
  snprintf(v0n, 32, "%s", v0);
  const char* v1 = irwriter_insertvalue_imm(w, "{i32, i32}", v0n, "i32", 0, 1);
  char v1n[32];
  snprintf(v1n, 32, "%s", v1);
  irwriter_ret(w, "{i32, i32}", v1n);

  irwriter_bb(w, "s0_fail");
  irwriter_dbg(w, 2, 10);
  const char* v2 = irwriter_insertvalue_imm(w, "{i32, i32}", "undef", "i32", 0, 0);
  char v2n[32];
  snprintf(v2n, 32, "%s", v2);
  const char* v3 = irwriter_insertvalue_imm(w, "{i32, i32}", v2n, "i32", -2, 1);
  char v3n[32];
  snprintf(v3n, 32, "%s", v3);
  irwriter_ret(w, "{i32, i32}", v3n);

  irwriter_bb(w, "dead");
  irwriter_dbg(w, 1, 1);
  const char* v4 = irwriter_insertvalue_imm(w, "{i32, i32}", "undef", "i32", 0, 0);
  char v4n[32];
  snprintf(v4n, 32, "%s", v4);
  const char* v5 = irwriter_insertvalue_imm(w, "{i32, i32}", v4n, "i32", -2, 1);
  char v5n[32];
  snprintf(v5n, 32, "%s", v5);
  irwriter_ret(w, "{i32, i32}", v5n);

  irwriter_define_end(w);
  irwriter_end(w);
  irwriter_del(w);
  fclose(f);

  char cmd[256];
  snprintf(cmd, sizeof(cmd), "xcrun clang -c %s -o %s 2>&1", ll_path, obj_path);
  FILE* p = popen(cmd, "r");
  assert(p);
  char output[4096] = {0};
  size_t n = fread(output, 1, sizeof(output) - 1, p);
  output[n] = '\0';
  int status = pclose(p);
  if (status != 0) {
    fprintf(stderr, "\nclang failed:\n%s\n", output);
    // dump the .ll for debugging
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
