#include "../src/darray.h"
#include "../src/re.h"
#include "compat.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

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

static void _build_ch_seq(Aut* a, Re* re, IrWriter* w) {
  re_append_ch(re, 'H', (DebugInfo){0, 0});
  re_append_ch(re, 'i', (DebugInfo){0, 0});
  re_action(re, 1);
  aut_gen_dfa(a, w, false);
}

static void _build_append_range(Aut* a, Re* re, IrWriter* w) {
  ReRange* r = re_range_new();
  re_range_add(r, 'A', 'Z');
  re_append_range(re, r, (DebugInfo){0, 0});
  re_range_del(r);
  re_action(re, 1);
  aut_gen_dfa(a, w, false);
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

static void _build_group(Aut* a, Re* re, IrWriter* w) {
  re_lparen(re);
  re_append_ch(re, 'a', (DebugInfo){0, 0});
  re_append_ch(re, 'b', (DebugInfo){0, 0});
  re_rparen(re);
  re_append_ch(re, 'c', (DebugInfo){0, 0});
  re_action(re, 1);
  aut_gen_dfa(a, w, false);
}

static void _build_alt(Aut* a, Re* re, IrWriter* w) {
  re_lparen(re);
  re_append_ch(re, 'a', (DebugInfo){0, 0});
  re_fork(re);
  re_append_ch(re, 'b', (DebugInfo){0, 0});
  re_rparen(re);
  re_action(re, 1);
  aut_gen_dfa(a, w, false);
}

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

static void _build_action(Aut* a, Re* re, IrWriter* w) {
  re_append_ch(re, 'x', (DebugInfo){0, 0});
  re_action(re, 42);
  aut_gen_dfa(a, w, false);
}

// --- Shared lib helpers ---

typedef struct {
  int64_t state;
  int64_t action;
} MatchResult;
typedef MatchResult (*MatchFn)(int64_t, int64_t);

typedef struct {
  MatchFn fn;
  void* handle;
  char ll_path[128];
  char lib_path[128];
} LoadedMatch;

static int _run_cmd(const char* cmd_str) {
  FILE* p = compat_popen(cmd_str, "r");
  if (!p) {
    return -1;
  }
  char buf[4096];
  size_t n = fread(buf, 1, sizeof(buf) - 1, p);
  buf[n] = '\0';
  int exit_code = compat_pclose(p);
  if (exit_code != 0) {
    fprintf(stderr, "cmd failed: %s\noutput: %s\n", cmd_str, buf);
  }
  return exit_code;
}

static LoadedMatch _load_match_re(void (*fn)(Aut*, Re*, IrWriter*), const char* test_name) {
  LoadedMatch lm = {0};
  snprintf(lm.ll_path, sizeof(lm.ll_path), "%s/test_re_exec_%s.ll", BUILD_DIR, test_name);
#ifdef __APPLE__
  snprintf(lm.lib_path, sizeof(lm.lib_path), "%s/test_re_exec_%s.dylib", BUILD_DIR, test_name);
#else
  snprintf(lm.lib_path, sizeof(lm.lib_path), "%s/test_re_exec_%s.so", BUILD_DIR, test_name);
#endif

  FILE* f = fopen(lm.ll_path, "w");
  assert(f);
  IrWriter* w = irwriter_new(f);
  irwriter_start(w, 5, "test.rules", ".");
  Aut* a = aut_new("match", "test.rules");
  Re* re = re_new(a);
  fn(a, re, w);
  re_del(re);
  aut_del(a);
  irwriter_end(w);
  irwriter_del(w);
  fclose(f);

  char cmd[512];
#ifdef __APPLE__
  snprintf(cmd, sizeof(cmd), "%s -dynamiclib -Wl,-undefined,dynamic_lookup -o %s %s 2>&1", LLVM_CC, lm.lib_path,
           lm.ll_path);
#else
  snprintf(cmd, sizeof(cmd), "%s -shared -o %s %s 2>&1", LLVM_CC, lm.lib_path, lm.ll_path);
#endif
  int status = _run_cmd(cmd);
  assert(status == 0);

#ifdef _WIN32
  lm.handle = LoadLibraryA(lm.lib_path);
  assert(lm.handle);
  lm.fn = (MatchFn)GetProcAddress(lm.handle, "match");
#else
  lm.handle = dlopen(lm.lib_path, RTLD_NOW);
  if (!lm.handle) {
    fprintf(stderr, "dlopen: %s\n", dlerror());
  }
  assert(lm.handle);
  lm.fn = (MatchFn)dlsym(lm.handle, "match");
#endif
  assert(lm.fn);
  return lm;
}

static void _unload(LoadedMatch* lm) {
#ifdef _WIN32
  FreeLibrary(lm->handle);
#else
  dlclose(lm->handle);
#endif
  remove(lm->ll_path);
  remove(lm->lib_path);
}

// --- Execution tests ---

TEST(test_exec_ch) {
  LoadedMatch lm = _load_match_re(_build_ch, "ch");
  MatchResult r = lm.fn(0, 'A');
  assert(r.action == 1);
  r = lm.fn(0, 'B');
  assert(r.action == -2);
  _unload(&lm);
}

TEST(test_exec_ch_seq) {
  LoadedMatch lm = _load_match_re(_build_ch_seq, "ch_seq");
  MatchResult r;
  // feed 'H' -> intermediate
  r = lm.fn(0, 'H');
  assert(r.action == 0);
  int64_t s1 = r.state;
  // feed 'i' -> action 1
  r = lm.fn(s1, 'i');
  assert(r.action == 1);
  // feed 'H' then wrong char -> dead
  r = lm.fn(0, 'H');
  s1 = r.state;
  r = lm.fn(s1, 'a');
  assert(r.action == -2);
  _unload(&lm);
}

TEST(test_exec_range) {
  LoadedMatch lm = _load_match_re(_build_append_range, "range");
  MatchResult r;
  r = lm.fn(0, 'A');
  assert(r.action == 1);
  r = lm.fn(0, 'Z');
  assert(r.action == 1);
  r = lm.fn(0, 'M');
  assert(r.action == 1);
  r = lm.fn(0, 'a');
  assert(r.action == -2);
  _unload(&lm);
}

TEST(test_exec_multi_range) {
  LoadedMatch lm = _load_match_re(_build_append_multi_range, "mrange");
  MatchResult r;
  r = lm.fn(0, 'A');
  assert(r.action == 1);
  r = lm.fn(0, 'z');
  assert(r.action == 1);
  r = lm.fn(0, 'a');
  assert(r.action == 1);
  r = lm.fn(0, 'Z');
  assert(r.action == 1);
  r = lm.fn(0, '0');
  assert(r.action == -2);
  _unload(&lm);
}

TEST(test_exec_group) {
  LoadedMatch lm = _load_match_re(_build_group, "group");
  MatchResult r;
  // feed 'a','b','c' -> action 1
  r = lm.fn(0, 'a');
  assert(r.action == 0);
  r = lm.fn(r.state, 'b');
  assert(r.action == 0);
  r = lm.fn(r.state, 'c');
  assert(r.action == 1);
  // feed 'a','b','d' -> dead on 'd'
  r = lm.fn(0, 'a');
  r = lm.fn(r.state, 'b');
  r = lm.fn(r.state, 'd');
  assert(r.action == -2);
  _unload(&lm);
}

TEST(test_exec_alt) {
  LoadedMatch lm = _load_match_re(_build_alt, "alt");
  MatchResult r;
  r = lm.fn(0, 'a');
  assert(r.action == 1);
  r = lm.fn(0, 'b');
  assert(r.action == 1);
  r = lm.fn(0, 'c');
  assert(r.action == -2);
  _unload(&lm);
}

TEST(test_exec_complex) {
  LoadedMatch lm = _load_match_re(_build_complex, "complex");
  MatchResult r;
  // 'a','b','e' -> action 1
  r = lm.fn(0, 'a');
  assert(r.action == 0);
  r = lm.fn(r.state, 'b');
  assert(r.action == 0);
  r = lm.fn(r.state, 'e');
  assert(r.action == 1);
  // 'c','d','e' -> action 1
  r = lm.fn(0, 'c');
  assert(r.action == 0);
  r = lm.fn(r.state, 'd');
  assert(r.action == 0);
  r = lm.fn(r.state, 'e');
  assert(r.action == 1);
  // 'a','d' -> dead
  r = lm.fn(0, 'a');
  r = lm.fn(r.state, 'd');
  assert(r.action == -2);
  _unload(&lm);
}

TEST(test_exec_action) {
  LoadedMatch lm = _load_match_re(_build_action, "action");
  MatchResult r = lm.fn(0, 'x');
  assert(r.action == 42);
  _unload(&lm);
}

static void _build_neg_range(Aut* a, Re* re, IrWriter* w) {
  // [^abc] => action 1
  ReRange* r = re_range_new();
  re_range_add(r, 'a', 'a');
  re_range_add(r, 'b', 'b');
  re_range_add(r, 'c', 'c');
  re_range_neg(r);
  re_append_range(re, r, (DebugInfo){0, 0});
  re_range_del(r);
  re_action(re, 1);
  aut_gen_dfa(a, w, false);
}

TEST(test_exec_neg_range) {
  LoadedMatch lm = _load_match_re(_build_neg_range, "neg_range");
  MatchResult r;
  // 'x','y','z' should match (not in [abc])
  r = lm.fn(0, 'x');
  assert(r.action == 1);
  r = lm.fn(0, 'y');
  assert(r.action == 1);
  r = lm.fn(0, 'z');
  assert(r.action == 1);
  r = lm.fn(0, '0');
  assert(r.action == 1);
  // 'a','b','c' should NOT match
  r = lm.fn(0, 'a');
  assert(r.action == -2);
  r = lm.fn(0, 'b');
  assert(r.action == -2);
  r = lm.fn(0, 'c');
  assert(r.action == -2);
  _unload(&lm);
}

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
  // execution tests
  RUN(test_exec_ch);
  RUN(test_exec_ch_seq);
  RUN(test_exec_range);
  RUN(test_exec_multi_range);
  RUN(test_exec_group);
  RUN(test_exec_alt);
  RUN(test_exec_complex);
  RUN(test_exec_action);
  RUN(test_exec_neg_range);
  printf("all ok\n");
  return 0;
}
