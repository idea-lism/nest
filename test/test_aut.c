#include "../src/aut.h"
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

static char* _gen_ir(void (*fn)(Aut*, IrWriter*)) {
  char* buf = NULL;
  size_t sz = 0;
  FILE* f = compat_open_memstream(&buf, &sz);
  assert(f);
  IrWriter* w = irwriter_new(f);
  irwriter_start(w, 5, "test.rules", ".");

  Aut* a = aut_new("match", "test.rules");
  fn(a, w);
  aut_del(a);

  irwriter_end(w);
  irwriter_del(w);
  compat_close_memstream(f, &buf, &sz);
  return buf;
}

// --- Basic: single transition ---

static void _build_single(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, 'A', 'A'}, (DebugInfo){1, 1});
  aut_epsilon(a, 1, 2);
  aut_action(a, 2, 1);
  aut_gen_dfa(a, w, false);
}

// --- Range transition ---

static void _build_range(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, 'A', 'Z'}, (DebugInfo){1, 1});
  aut_epsilon(a, 1, 2);
  aut_action(a, 2, 1);
  aut_gen_dfa(a, w, false);
}

// --- Multiple transitions from one state ---

static void _build_multi(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, 'A', 'Z'}, (DebugInfo){1, 1});
  aut_epsilon(a, 1, 3);
  aut_action(a, 3, 1);
  aut_transition(a, (TransitionDef){0, 2, 'a', 'z'}, (DebugInfo){2, 1});
  aut_epsilon(a, 2, 4);
  aut_action(a, 4, 2);
  aut_gen_dfa(a, w, false);
}

// --- Epsilon transitions ---

static void _build_epsilon(Aut* a, IrWriter* w) {
  aut_epsilon(a, 0, 1);
  aut_transition(a, (TransitionDef){1, 2, 'x', 'x'}, (DebugInfo){1, 1});
  aut_epsilon(a, 2, 3);
  aut_action(a, 3, 1);
  aut_gen_dfa(a, w, false);
}

// --- Special codepoints ---

static void _build_special_cp(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, 0, 0}, (DebugInfo){1, 1});
  aut_transition(a, (TransitionDef){1, 2, 'a', 'z'}, (DebugInfo){1, 5});
  aut_epsilon(a, 2, 3);
  aut_action(a, 3, 1);
  aut_transition(a, (TransitionDef){3, 4, 0x10FFFF, 0x10FFFF}, (DebugInfo){1, 10});
  aut_epsilon(a, 4, 5);
  aut_action(a, 5, 2);
  aut_gen_dfa(a, w, false);
}

// --- Action ID: smallest returned ---

static void _build_action_smallest(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, 'A', 'Z'}, (DebugInfo){1, 1});
  aut_epsilon(a, 1, 3);
  aut_action(a, 3, 5);
  aut_transition(a, (TransitionDef){0, 2, 'M', 'M'}, (DebugInfo){1, 5});
  aut_epsilon(a, 2, 4);
  aut_action(a, 4, 3);
  aut_gen_dfa(a, w, false);
}

// --- Debug info ---

static void _build_debug(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, 'A', 'A'}, (DebugInfo){10, 5});
  aut_epsilon(a, 1, 2);
  aut_action(a, 2, 1);
  aut_gen_dfa(a, w, false);
}

TEST(test_debug_info) {
  char* out = _gen_ir(_build_debug);
  assert(strstr(out, "DILocation(line: 10, column: 5"));
  assert(strstr(out, "DISubprogram(name: \"match\""));
  free(out);
}

// --- Debug trap on dead state ---

static void _build_debug_trap(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, 'A', 'A'}, (DebugInfo){1, 1});
  aut_epsilon(a, 1, 2);
  aut_action(a, 2, 1);
  aut_gen_dfa(a, w, true);
}

TEST(test_debug_trap) {
  char* out = _gen_ir(_build_debug_trap);
  assert(strstr(out, "declare void @llvm.debugtrap()"));
  assert(strstr(out, "call void @llvm.debugtrap()"));
  free(out);
}

// --- Optimize (Brzozowski) ---

static void _build_redundant(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, 'a', 'a'}, (DebugInfo){1, 1});
  aut_transition(a, (TransitionDef){0, 2, 'b', 'b'}, (DebugInfo){1, 5});
  aut_transition(a, (TransitionDef){1, 3, 'c', 'c'}, (DebugInfo){2, 1});
  aut_action(a, 3, 1);
  aut_transition(a, (TransitionDef){2, 4, 'c', 'c'}, (DebugInfo){2, 5});
  aut_action(a, 4, 1);
  aut_optimize(a);
  aut_gen_dfa(a, w, false);
}

TEST(test_optimize_reduces_states) {
  Aut* a1 = aut_new("m", "test.rules");
  aut_transition(a1, (TransitionDef){0, 1, 'a', 'a'}, (DebugInfo){1, 1});
  aut_transition(a1, (TransitionDef){0, 2, 'b', 'b'}, (DebugInfo){1, 5});
  aut_transition(a1, (TransitionDef){1, 3, 'c', 'c'}, (DebugInfo){2, 1});
  aut_action(a1, 3, 1);
  aut_transition(a1, (TransitionDef){2, 4, 'c', 'c'}, (DebugInfo){2, 5});
  aut_action(a1, 4, 1);
  int32_t unoptimized = aut_dfa_nstates(a1);
  aut_del(a1);

  Aut* a2 = aut_new("m", "test.rules");
  aut_transition(a2, (TransitionDef){0, 1, 'a', 'a'}, (DebugInfo){1, 1});
  aut_transition(a2, (TransitionDef){0, 2, 'b', 'b'}, (DebugInfo){1, 5});
  aut_transition(a2, (TransitionDef){1, 3, 'c', 'c'}, (DebugInfo){2, 1});
  aut_action(a2, 3, 1);
  aut_transition(a2, (TransitionDef){2, 4, 'c', 'c'}, (DebugInfo){2, 5});
  aut_action(a2, 4, 1);
  aut_optimize(a2);
  int32_t optimized = aut_dfa_nstates(a2);
  aut_del(a2);

  assert(optimized < unoptimized);
}

// --- aut_action basic test ---

static void _build_action_basic(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, 'x', 'x'}, (DebugInfo){1, 1});
  aut_action(a, 1, 7);
  aut_gen_dfa(a, w, false);
}

// --- MIN-RULE: multiple action_ids on same state, smallest wins ---

static void _build_min_rule(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, 'x', 'x'}, (DebugInfo){1, 1});
  aut_action(a, 1, 10);
  aut_action(a, 1, 3);
  aut_action(a, 1, 7);
  aut_gen_dfa(a, w, false);
}

// --- PRESERVING-RULE: action_ids survive optimization ---

static void _build_preserve(Aut* a, IrWriter* w) {
  // 0 --'a'--> 1 (action 2), 0 --'b'--> 2 (action 2)
  // 1 --'c'--> 3, 2 --'c'--> 4 (both with same action)
  // States 1 and 2 are equivalent and should merge, but action_id=2 must survive.
  aut_transition(a, (TransitionDef){0, 1, 'a', 'a'}, (DebugInfo){1, 1});
  aut_action(a, 1, 2);
  aut_transition(a, (TransitionDef){0, 2, 'b', 'b'}, (DebugInfo){1, 5});
  aut_action(a, 2, 2);
  aut_transition(a, (TransitionDef){1, 3, 'c', 'c'}, (DebugInfo){2, 1});
  aut_action(a, 3, 5);
  aut_transition(a, (TransitionDef){2, 4, 'c', 'c'}, (DebugInfo){2, 5});
  aut_action(a, 4, 5);
  aut_optimize(a);
  aut_gen_dfa(a, w, false);
}

// --- Optimize coalesces adjacent ranges from merged states ---

static void _build_coalesce(Aut* a, IrWriter* w) {
  // 0 --[a-m]--> 1, 0 --[n-z]--> 2
  // 1 --[x]--> 3 (action 1), 2 --[x]--> 4 (action 1)
  // States 1 and 2 are equivalent. After minimization they merge,
  // so transitions [a-m] and [n-z] from state 0 both target the same
  // merged state and should coalesce into [a-z].
  aut_transition(a, (TransitionDef){0, 1, 'a', 'm'}, (DebugInfo){1, 1});
  aut_transition(a, (TransitionDef){0, 2, 'n', 'z'}, (DebugInfo){1, 5});
  aut_transition(a, (TransitionDef){1, 3, 'x', 'x'}, (DebugInfo){2, 1});
  aut_action(a, 3, 1);
  aut_transition(a, (TransitionDef){2, 4, 'x', 'x'}, (DebugInfo){2, 5});
  aut_action(a, 4, 1);
  aut_optimize(a);
  aut_gen_dfa(a, w, false);
}

TEST(test_optimize_coalesces_ranges) {
  char* out = _gen_ir(_build_coalesce);
  // After coalescing, [a-z] = [97, 122] should appear as a single range check.
  // 97 = 'a', 122 = 'z'. The IR should have "sge i64 %cp, 97" and "sle i64 %cp, 122".
  assert(strstr(out, "97"));
  assert(strstr(out, "122"));
  // The split boundaries 109 ('m') and 110 ('n') should NOT appear — they were coalesced.
  assert(!strstr(out, "109"));
  assert(!strstr(out, "110"));
  free(out);
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

static LoadedMatch _load_match(void (*fn)(Aut*, IrWriter*), const char* test_name) {
  LoadedMatch lm = {0};
  snprintf(lm.ll_path, sizeof(lm.ll_path), "%s/test_aut_exec_%s.ll", BUILD_DIR, test_name);
#ifdef __APPLE__
  snprintf(lm.lib_path, sizeof(lm.lib_path), "%s/test_aut_exec_%s.dylib", BUILD_DIR, test_name);
#else
  snprintf(lm.lib_path, sizeof(lm.lib_path), "%s/test_aut_exec_%s.so", BUILD_DIR, test_name);
#endif

  FILE* f = fopen(lm.ll_path, "w");
  assert(f);
  IrWriter* w = irwriter_new(f);
  irwriter_start(w, 5, "test.rules", ".");
  Aut* a = aut_new("match", "test.rules");
  fn(a, w);
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
  if (status != 0) {
    fprintf(stderr, "shared lib compile failed for %s\n", test_name);
  }
  assert(status == 0);

#ifdef _WIN32
  lm.handle = LoadLibraryA(lm.lib_path);
  assert(lm.handle);
  lm.fn = (MatchFn)GetProcAddress(lm.handle, "match");
#else
  lm.handle = dlopen(lm.lib_path, RTLD_NOW);
  if (!lm.handle) {
    fprintf(stderr, "dlopen failed: %s\n", dlerror());
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

// --- Clang compilation ---

static void _write_and_compile(void (*fn)(Aut*, IrWriter*), const char* test_name) {
  char ll_path[128], obj_path[128];
  snprintf(ll_path, sizeof(ll_path), "%s/test_aut_%s.ll", BUILD_DIR, test_name);
  snprintf(obj_path, sizeof(obj_path), "%s/test_aut_%s.o", BUILD_DIR, test_name);

  FILE* f = fopen(ll_path, "w");
  if (!f) {
    fprintf(stderr, "fopen failed: %s\n", ll_path);
  }
  assert(f);
  IrWriter* w = irwriter_new(f);
  irwriter_start(w, 5, "test.rules", ".");

  Aut* a = aut_new("match", "test.rules");
  fn(a, w);
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
  int status = compat_pclose(p);
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

TEST(test_compile_special) { _write_and_compile(_build_special_cp, "special"); }

TEST(test_compile_debug) { _write_and_compile(_build_debug, "debug"); }

TEST(test_compile_debug_trap) { _write_and_compile(_build_debug_trap, "debug_trap"); }

// --- Execution tests ---

TEST(test_exec_single) {
  LoadedMatch lm = _load_match(_build_single, "exec_single");
  MatchResult r = lm.fn(0, 'A');
  assert(r.action == 1);
  assert(r.state > 0);
  r = lm.fn(0, 'B');
  assert(r.action == -2);
  _unload(&lm);
}

TEST(test_exec_range) {
  LoadedMatch lm = _load_match(_build_range, "exec_range");
  MatchResult r;
  r = lm.fn(0, 'A');
  assert(r.action == 1);
  r = lm.fn(0, 'M');
  assert(r.action == 1);
  r = lm.fn(0, 'Z');
  assert(r.action == 1);
  r = lm.fn(0, 'a');
  assert(r.action == -2);
  r = lm.fn(0, '0');
  assert(r.action == -2);
  _unload(&lm);
}

TEST(test_exec_multi) {
  LoadedMatch lm = _load_match(_build_multi, "exec_multi");
  MatchResult r;
  // uppercase A-Z -> action 1
  r = lm.fn(0, 'A');
  assert(r.action == 1);
  r = lm.fn(0, 'Z');
  assert(r.action == 1);
  // lowercase a-z -> action 2
  r = lm.fn(0, 'a');
  assert(r.action == 2);
  r = lm.fn(0, 'z');
  assert(r.action == 2);
  // digit -> dead
  r = lm.fn(0, '0');
  assert(r.action == -2);
  _unload(&lm);
}

TEST(test_exec_epsilon) {
  LoadedMatch lm = _load_match(_build_epsilon, "exec_eps");
  MatchResult r;
  r = lm.fn(0, 'x');
  assert(r.action == 1);
  r = lm.fn(0, 'y');
  assert(r.action == -2);
  _unload(&lm);
}

TEST(test_exec_action_basic) {
  LoadedMatch lm = _load_match(_build_action_basic, "exec_act");
  MatchResult r = lm.fn(0, 'x');
  assert(r.action == 7);
  _unload(&lm);
}

TEST(test_exec_min_rule) {
  LoadedMatch lm = _load_match(_build_min_rule, "exec_min");
  MatchResult r = lm.fn(0, 'x');
  assert(r.action == 3); // min(10,3,7)
  _unload(&lm);
}

TEST(test_exec_action_smallest) {
  LoadedMatch lm = _load_match(_build_action_smallest, "exec_smallest");
  MatchResult r;
  // 'M' matches both [A-Z](action 5) and [M](action 3) -> MIN-RULE picks 3
  r = lm.fn(0, 'M');
  assert(r.action == 3);
  // 'A' matches only [A-Z](action 5)
  r = lm.fn(0, 'A');
  assert(r.action == 5);
  _unload(&lm);
}

TEST(test_exec_optimize) {
  LoadedMatch lm = _load_match(_build_redundant, "exec_opt");
  MatchResult r;
  // 'a' then 'c' -> action 1
  r = lm.fn(0, 'a');
  assert(r.action == 0); // intermediate
  int64_t s1 = r.state;
  r = lm.fn(s1, 'c');
  assert(r.action == 1);
  // 'b' then 'c' -> action 1
  r = lm.fn(0, 'b');
  assert(r.action == 0);
  s1 = r.state;
  r = lm.fn(s1, 'c');
  assert(r.action == 1);
  // 'a' then 'x' -> dead
  r = lm.fn(0, 'a');
  s1 = r.state;
  r = lm.fn(s1, 'x');
  assert(r.action == -2);
  _unload(&lm);
}

TEST(test_exec_coalesce) {
  LoadedMatch lm = _load_match(_build_coalesce, "exec_coal");
  MatchResult r;
  // any a-z then 'x' -> action 1
  const char* letters = "abcmnoxyz";
  for (int32_t i = 0; letters[i]; i++) {
    r = lm.fn(0, letters[i]);
    assert(r.action == 0); // intermediate
    int64_t s1 = r.state;
    r = lm.fn(s1, 'x');
    assert(r.action == 1);
  }
  // digit -> dead
  r = lm.fn(0, '0');
  assert(r.action == -2);
  _unload(&lm);
}

TEST(test_exec_preserve) {
  LoadedMatch lm = _load_match(_build_preserve, "exec_prsv");
  MatchResult r;
  // 'a' -> action 2
  r = lm.fn(0, 'a');
  assert(r.action == 2);
  // 'b' -> action 2
  r = lm.fn(0, 'b');
  assert(r.action == 2);
  // 'a' then 'c' -> action 5
  r = lm.fn(0, 'a');
  int64_t s1 = r.state;
  r = lm.fn(s1, 'c');
  assert(r.action == 5);
  _unload(&lm);
}

// --- Lifecycle ---

TEST(test_lifecycle) {
  Aut* a = aut_new("f", "test.rules");
  assert(a);
  aut_del(a);
}

TEST(test_empty_aut) {
  char* buf = NULL;
  size_t sz = 0;
  FILE* f = compat_open_memstream(&buf, &sz);
  assert(f);
  IrWriter* w = irwriter_new(f);
  irwriter_start(w, 5, "test.rules", ".");

  Aut* a = aut_new("empty", "test.rules");
  aut_gen_dfa(a, w, false);
  aut_del(a);

  irwriter_end(w);
  irwriter_del(w);
  compat_close_memstream(f, &buf, &sz);

  assert(strstr(buf, "define {i64, i64} @empty"));
  assert(strstr(buf, "L"));
  free(buf);
}

int main(void) {
  printf("test_aut:\n");
  RUN(test_lifecycle);
  RUN(test_empty_aut);
  RUN(test_debug_info);
  RUN(test_optimize_reduces_states);
  RUN(test_optimize_coalesces_ranges);
  RUN(test_compile_special);
  RUN(test_compile_debug);
  RUN(test_debug_trap);
  RUN(test_compile_debug_trap);
  // execution tests
  RUN(test_exec_single);
  RUN(test_exec_range);
  RUN(test_exec_multi);
  RUN(test_exec_epsilon);
  RUN(test_exec_action_basic);
  RUN(test_exec_min_rule);
  RUN(test_exec_action_smallest);
  RUN(test_exec_optimize);
  RUN(test_exec_coalesce);
  RUN(test_exec_preserve);
  printf("all ok\n");
  return 0;
}
