#include "../src/darray.h"
#include "../src/re.h"
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

static char* _gen_ir(void (*fn)(Aut*, Re*, IrWriter*)) {
  char* buf = NULL;
  size_t sz = 0;
  FILE* f = compat_open_memstream(&buf, &sz);
  assert(f);
  IrWriter* w = irwriter_new(f, TARGET);
  irwriter_start(w, "test.rules", ".");

  Aut* a = aut_new("match", "test.rules");
  Re* re = re_new(a);
  fn(a, re, w);
  re_del(re);
  aut_del(a);

  irwriter_end(w);
  irwriter_del(w);
  compat_close_memstream(f, &buf, &sz);
  return buf;
}

// --- Lifecycle ---

TEST(test_lifecycle) {
  Aut* a = aut_new("f", "test.rules");
  Re* re = re_new(a);
  assert(re);
  re_del(re);
  aut_del(a);
}

TEST(test_range_lifecycle) {
  ReRange* r = re_range_new();
  assert(r);
  assert(darray_size(r->ivs) == 0);
  re_range_del(r);
}

// --- ReRange add ---

TEST(test_range_add_single) {
  ReRange* r = re_range_new();
  re_range_add(r, 10, 20);
  assert(darray_size(r->ivs) == 1);
  assert(r->ivs[0].start == 10);
  assert(r->ivs[0].end == 20);
  re_range_del(r);
}

TEST(test_range_add_disjoint) {
  ReRange* r = re_range_new();
  re_range_add(r, 10, 20);
  re_range_add(r, 30, 40);
  assert(darray_size(r->ivs) == 2);
  assert(r->ivs[0].start == 10 && r->ivs[0].end == 20);
  assert(r->ivs[1].start == 30 && r->ivs[1].end == 40);
  re_range_del(r);
}

TEST(test_range_add_disjoint_unsorted) {
  ReRange* r = re_range_new();
  re_range_add(r, 30, 40);
  re_range_add(r, 10, 20);
  assert(darray_size(r->ivs) == 2);
  assert(r->ivs[0].start == 10 && r->ivs[0].end == 20);
  assert(r->ivs[1].start == 30 && r->ivs[1].end == 40);
  re_range_del(r);
}

TEST(test_range_add_overlap) {
  ReRange* r = re_range_new();
  re_range_add(r, 10, 20);
  re_range_add(r, 15, 30);
  assert(darray_size(r->ivs) == 1);
  assert(r->ivs[0].start == 10 && r->ivs[0].end == 30);
  re_range_del(r);
}

TEST(test_range_add_adjacent) {
  ReRange* r = re_range_new();
  re_range_add(r, 10, 20);
  re_range_add(r, 21, 30);
  assert(darray_size(r->ivs) == 1);
  assert(r->ivs[0].start == 10 && r->ivs[0].end == 30);
  re_range_del(r);
}

TEST(test_range_add_subset) {
  ReRange* r = re_range_new();
  re_range_add(r, 10, 40);
  re_range_add(r, 15, 25);
  assert(darray_size(r->ivs) == 1);
  assert(r->ivs[0].start == 10 && r->ivs[0].end == 40);
  re_range_del(r);
}

TEST(test_range_add_superset) {
  ReRange* r = re_range_new();
  re_range_add(r, 15, 25);
  re_range_add(r, 10, 40);
  assert(darray_size(r->ivs) == 1);
  assert(r->ivs[0].start == 10 && r->ivs[0].end == 40);
  re_range_del(r);
}

TEST(test_range_add_merge_three) {
  ReRange* r = re_range_new();
  re_range_add(r, 10, 20);
  re_range_add(r, 30, 40);
  re_range_add(r, 50, 60);
  assert(darray_size(r->ivs) == 3);
  // now merge all three
  re_range_add(r, 15, 55);
  assert(darray_size(r->ivs) == 1);
  assert(r->ivs[0].start == 10 && r->ivs[0].end == 60);
  re_range_del(r);
}

// --- ReRange neg ---

TEST(test_range_neg_empty) {
  ReRange* r = re_range_new();
  re_range_neg(r);
  assert(darray_size(r->ivs) == 1);
  assert(r->ivs[0].start == 0 && r->ivs[0].end == 0x10FFFF);
  re_range_del(r);
}

TEST(test_range_neg_full) {
  ReRange* r = re_range_new();
  re_range_add(r, 0, 0x10FFFF);
  re_range_neg(r);
  assert(darray_size(r->ivs) == 0);
  re_range_del(r);
}

TEST(test_range_neg_single) {
  ReRange* r = re_range_new();
  re_range_add(r, 10, 20);
  re_range_neg(r);
  assert(darray_size(r->ivs) == 2);
  assert(r->ivs[0].start == 0 && r->ivs[0].end == 9);
  assert(r->ivs[1].start == 21 && r->ivs[1].end == 0x10FFFF);
  re_range_del(r);
}

TEST(test_range_neg_at_start) {
  ReRange* r = re_range_new();
  re_range_add(r, 0, 5);
  re_range_neg(r);
  assert(darray_size(r->ivs) == 1);
  assert(r->ivs[0].start == 6 && r->ivs[0].end == 0x10FFFF);
  re_range_del(r);
}

TEST(test_range_neg_at_end) {
  ReRange* r = re_range_new();
  re_range_add(r, 0x10FFFE, 0x10FFFF);
  re_range_neg(r);
  assert(darray_size(r->ivs) == 1);
  assert(r->ivs[0].start == 0 && r->ivs[0].end == 0x10FFFD);
  re_range_del(r);
}

TEST(test_range_neg_multiple) {
  ReRange* r = re_range_new();
  re_range_add(r, 5, 10);
  re_range_add(r, 20, 30);
  re_range_neg(r);
  assert(darray_size(r->ivs) == 3);
  assert(r->ivs[0].start == 0 && r->ivs[0].end == 4);
  assert(r->ivs[1].start == 11 && r->ivs[1].end == 19);
  assert(r->ivs[2].start == 31 && r->ivs[2].end == 0x10FFFF);
  re_range_del(r);
}

TEST(test_range_neg_double) {
  // negate twice = original
  ReRange* r = re_range_new();
  re_range_add(r, 5, 10);
  re_range_add(r, 20, 30);
  re_range_neg(r);
  re_range_neg(r);
  assert(darray_size(r->ivs) == 2);
  assert(r->ivs[0].start == 5 && r->ivs[0].end == 10);
  assert(r->ivs[1].start == 20 && r->ivs[1].end == 30);
  re_range_del(r);
}

// --- re_append_ch ---

static void _build_ch(Aut* a, Re* re, IrWriter* w) {
  re_append_ch(re, 'A', (DebugInfo){0, 0});
  re_action(re, 1);
  aut_gen_dfa(a, w, false);
}

TEST(test_append_ch) {
  char* out = _gen_ir(_build_ch);
  assert(strstr(out, "i32 65")); // 'A'
  free(out);
}

static void _build_ch_seq(Aut* a, Re* re, IrWriter* w) {
  re_append_ch(re, 'H', (DebugInfo){0, 0});
  re_append_ch(re, 'i', (DebugInfo){0, 0});
  re_action(re, 1);
  aut_gen_dfa(a, w, false);
}

TEST(test_append_ch_seq) {
  char* out = _gen_ir(_build_ch_seq);
  assert(strstr(out, "i32 72"));  // H
  assert(strstr(out, "i32 105")); // i
  free(out);
}

// --- re_append_range ---

static void _build_append_range(Aut* a, Re* re, IrWriter* w) {
  ReRange* r = re_range_new();
  re_range_add(r, 'A', 'Z');
  re_append_range(re, r, (DebugInfo){0, 0});
  re_range_del(r);
  re_action(re, 1);
  aut_gen_dfa(a, w, false);
}

TEST(test_append_range) {
  char* out = _gen_ir(_build_append_range);
  assert(strstr(out, "icmp sge i32 %r"));
  assert(strstr(out, "icmp sle i32 %r"));
  free(out);
}

static void _build_append_multi_range(Aut* a, Re* re, IrWriter* w) {
  ReRange* r = re_range_new();
  re_range_add(r, 'A', 'Z');
  re_range_add(r, 'a', 'z');
  re_append_range(re, r, (DebugInfo){0, 0});
  re_range_del(r);
  re_action(re, 1);
  aut_gen_dfa(a, w, false);
}

TEST(test_append_multi_range) {
  char* out = _gen_ir(_build_append_multi_range);
  // both ranges present
  assert(strstr(out, "icmp sge i32 %r"));
  assert(strstr(out, "icmp sle i32 %r"));
  assert(strstr(out, "icmp sge i32 %r"));
  assert(strstr(out, "icmp sle i32 %r"));
  free(out);
}

// --- re_lparen / re_rparen ---

static void _build_group(Aut* a, Re* re, IrWriter* w) {
  re_lparen(re);
  re_append_ch(re, 'a', (DebugInfo){0, 0});
  re_append_ch(re, 'b', (DebugInfo){0, 0});
  re_rparen(re);
  re_append_ch(re, 'c', (DebugInfo){0, 0});
  re_action(re, 1);
  aut_gen_dfa(a, w, false);
}

TEST(test_group) {
  char* out = _gen_ir(_build_group);
  assert(strstr(out, "i32 97"));
  assert(strstr(out, "i32 98"));
  assert(strstr(out, "i32 99"));
  free(out);
}

// --- re_fork ---

static void _build_alt(Aut* a, Re* re, IrWriter* w) {
  re_lparen(re);
  re_append_ch(re, 'a', (DebugInfo){0, 0});
  re_fork(re);
  re_append_ch(re, 'b', (DebugInfo){0, 0});
  re_rparen(re);
  re_action(re, 1);
  aut_gen_dfa(a, w, false);
}

TEST(test_alt) {
  char* out = _gen_ir(_build_alt);
  assert(strstr(out, "i32 97"));
  assert(strstr(out, "i32 98"));
  free(out);
}

// --- Complex: (ab|cd)e ---

static void _build_complex(Aut* a, Re* re, IrWriter* w) {
  re_lparen(re);
  re_append_ch(re, 'a', (DebugInfo){0, 0});
  re_append_ch(re, 'b', (DebugInfo){0, 0});
  re_fork(re);
  re_append_ch(re, 'c', (DebugInfo){0, 0});
  re_append_ch(re, 'd', (DebugInfo){0, 0});
  re_rparen(re);
  re_append_ch(re, 'e', (DebugInfo){0, 0});
  re_action(re, 1);
  aut_optimize(a);
  aut_gen_dfa(a, w, false);
}

TEST(test_complex) {
  char* out = _gen_ir(_build_complex);
  assert(strstr(out, "define {i64, i64} @match"));
  assert(strstr(out, "i32 97"));  // a
  assert(strstr(out, "i32 98"));  // b
  assert(strstr(out, "i32 99"));  // c
  assert(strstr(out, "i32 100")); // d
  assert(strstr(out, "i32 101")); // e
  free(out);
}

// --- re_action ---

static void _build_action(Aut* a, Re* re, IrWriter* w) {
  re_append_ch(re, 'x', (DebugInfo){0, 0});
  re_action(re, 42);
  aut_gen_dfa(a, w, false);
}

TEST(test_action) {
  char* out = _gen_ir(_build_action);
  assert(strstr(out, "add i32 0, 42"));
  free(out);
}

// --- Clang compilation ---

static void _write_and_compile(void (*fn)(Aut*, Re*, IrWriter*), const char* test_name) {
  char ll_path[128], obj_path[128];
  snprintf(ll_path, sizeof(ll_path), "%s/test_re_%s.ll", BUILD_DIR, test_name);
  snprintf(obj_path, sizeof(obj_path), "%s/test_re_%s.o", BUILD_DIR, test_name);

  FILE* f = fopen(ll_path, "w");
  assert(f);
  IrWriter* w = irwriter_new(f, TARGET);
  irwriter_start(w, "test.rules", ".");

  Aut* a = aut_new("match", "test.rules");
  Re* re = re_new(a);
  fn(a, re, w);
  re_del(re);
  aut_del(a);

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
  int32_t status = compat_pclose(p);
  if (status != 0) {
    fprintf(stderr, "\nclang failed for %s:\n%s\n", test_name, output);
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

TEST(test_compile_ch) { _write_and_compile(_build_ch, "ch"); }
TEST(test_compile_ch_seq) { _write_and_compile(_build_ch_seq, "ch_seq"); }
TEST(test_compile_range) { _write_and_compile(_build_append_range, "range"); }
TEST(test_compile_multi_range) { _write_and_compile(_build_append_multi_range, "multi_range"); }
TEST(test_compile_group) { _write_and_compile(_build_group, "group"); }
TEST(test_compile_alt) { _write_and_compile(_build_alt, "alt"); }
TEST(test_compile_complex) { _write_and_compile(_build_complex, "complex"); }
TEST(test_compile_action) { _write_and_compile(_build_action, "action"); }

int main(void) {
  printf("test_re:\n");
  RUN(test_lifecycle);
  RUN(test_range_lifecycle);
  RUN(test_range_add_single);
  RUN(test_range_add_disjoint);
  RUN(test_range_add_disjoint_unsorted);
  RUN(test_range_add_overlap);
  RUN(test_range_add_adjacent);
  RUN(test_range_add_subset);
  RUN(test_range_add_superset);
  RUN(test_range_add_merge_three);
  RUN(test_range_neg_empty);
  RUN(test_range_neg_full);
  RUN(test_range_neg_single);
  RUN(test_range_neg_at_start);
  RUN(test_range_neg_at_end);
  RUN(test_range_neg_multiple);
  RUN(test_range_neg_double);
  RUN(test_append_ch);
  RUN(test_append_ch_seq);
  RUN(test_append_range);
  RUN(test_append_multi_range);
  RUN(test_group);
  RUN(test_alt);
  RUN(test_complex);
  RUN(test_action);
  RUN(test_compile_ch);
  RUN(test_compile_ch_seq);
  RUN(test_compile_range);
  RUN(test_compile_multi_range);
  RUN(test_compile_group);
  RUN(test_compile_alt);
  RUN(test_compile_complex);
  RUN(test_compile_action);
  printf("all ok\n");
  return 0;
}
