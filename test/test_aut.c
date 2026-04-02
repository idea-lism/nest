#include "../src/aut.h"
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

static char* _gen_ir(void (*fn)(Aut*, IrWriter*)) {
  char* buf = NULL;
  size_t sz = 0;
  FILE* f = compat_open_memstream(&buf, &sz);
  assert(f);
  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "test.rules", ".");

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

TEST(test_single_transition) {
  char* out = _gen_ir(_build_single);
  assert(strstr(out, "define {i64, i64} @match(i64 %state_i64, i64 %cp_i64)"));
  assert(strstr(out, "switch i32 %state, label %L"));
  assert(strstr(out, "i32 65, label %L"));
  assert(strstr(out, "L"));
  free(out);
}

// --- Range transition ---

static void _build_range(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, 'A', 'Z'}, (DebugInfo){1, 1});
  aut_epsilon(a, 1, 2);
  aut_action(a, 2, 1);
  aut_gen_dfa(a, w, false);
}

TEST(test_range_transition) {
  char* out = _gen_ir(_build_range);
  assert(strstr(out, "icmp sge i32 %cp"));
  assert(strstr(out, "icmp sle i32 %cp"));
  free(out);
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

TEST(test_multi_transitions) {
  char* out = _gen_ir(_build_multi);
  assert(strstr(out, "icmp sge i32 %cp"));
  assert(strstr(out, "icmp sle i32 %cp"));
  assert(strstr(out, "icmp sge i32 %cp"));
  assert(strstr(out, "icmp sle i32 %cp"));
  free(out);
}

// --- Epsilon transitions ---

static void _build_epsilon(Aut* a, IrWriter* w) {
  aut_epsilon(a, 0, 1);
  aut_transition(a, (TransitionDef){1, 2, 'x', 'x'}, (DebugInfo){1, 1});
  aut_epsilon(a, 2, 3);
  aut_action(a, 3, 1);
  aut_gen_dfa(a, w, false);
}

TEST(test_epsilon) {
  char* out = _gen_ir(_build_epsilon);
  assert(strstr(out, "i32 120"));
  free(out);
}

// --- Special codepoints ---

static void _build_special_cp(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, -1, -1}, (DebugInfo){1, 1});
  aut_transition(a, (TransitionDef){1, 2, 'a', 'z'}, (DebugInfo){1, 5});
  aut_epsilon(a, 2, 3);
  aut_action(a, 3, 1);
  aut_transition(a, (TransitionDef){3, 4, -2, -2}, (DebugInfo){1, 10});
  aut_epsilon(a, 4, 5);
  aut_action(a, 5, 2);
  aut_gen_dfa(a, w, false);
}

TEST(test_special_codepoints) {
  char* out = _gen_ir(_build_special_cp);
  assert(strstr(out, "-1"));
  assert(strstr(out, "-2"));
  free(out);
}

// --- Dead state ---

TEST(test_dead_state) {
  char* out = _gen_ir(_build_single);
  assert(strstr(out, "L"));
  free(out);
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

TEST(test_action_smallest) {
  char* out = _gen_ir(_build_action_smallest);
  assert(strstr(out, "i32 3, 1"));
  free(out);
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

TEST(test_optimize) {
  char* out = _gen_ir(_build_redundant);
  assert(strstr(out, "define {i64, i64} @match"));
  assert(strstr(out, "switch i32 %state, label %L"));
  free(out);
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

TEST(test_action_basic) {
  char* out = _gen_ir(_build_action_basic);
  assert(strstr(out, "i32 7, 1"));
  free(out);
}

// --- MIN-RULE: multiple action_ids on same state, smallest wins ---

static void _build_min_rule(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, 'x', 'x'}, (DebugInfo){1, 1});
  aut_action(a, 1, 10);
  aut_action(a, 1, 3);
  aut_action(a, 1, 7);
  aut_gen_dfa(a, w, false);
}

TEST(test_min_rule) {
  char* out = _gen_ir(_build_min_rule);
  assert(strstr(out, "i32 3, 1"));
  free(out);
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

TEST(test_preserving_rule) {
  char* out = _gen_ir(_build_preserve);
  assert(strstr(out, "i32 2, 1"));
  assert(strstr(out, "i32 5, 1"));
  free(out);
}

// --- Optimize reduces states AND preserves actions ---

TEST(test_optimize_preserves_action) {
  // Without optimization
  char* unopt = NULL;
  {
    char* buf = NULL;
    size_t sz = 0;
    FILE* f = compat_open_memstream(&buf, &sz);
    assert(f);
    IrWriter* w = irwriter_new(f, NULL);
    irwriter_start(w, "test.rules", ".");
    Aut* a = aut_new("match", "test.rules");
    aut_transition(a, (TransitionDef){0, 1, 'a', 'a'}, (DebugInfo){1, 1});
    aut_action(a, 1, 3);
    aut_transition(a, (TransitionDef){0, 2, 'b', 'b'}, (DebugInfo){1, 5});
    aut_action(a, 2, 3);
    aut_transition(a, (TransitionDef){1, 3, 'd', 'd'}, (DebugInfo){2, 1});
    aut_action(a, 3, 4);
    aut_transition(a, (TransitionDef){2, 4, 'd', 'd'}, (DebugInfo){2, 5});
    aut_action(a, 4, 4);
    aut_gen_dfa(a, w, false);
    aut_del(a);
    irwriter_end(w);
    irwriter_del(w);
    compat_close_memstream(f, &buf, &sz);
    unopt = buf;
  }

  // With optimization
  char* opt = NULL;
  {
    char* buf = NULL;
    size_t sz = 0;
    FILE* f = compat_open_memstream(&buf, &sz);
    assert(f);
    IrWriter* w = irwriter_new(f, NULL);
    irwriter_start(w, "test.rules", ".");
    Aut* a = aut_new("match", "test.rules");
    aut_transition(a, (TransitionDef){0, 1, 'a', 'a'}, (DebugInfo){1, 1});
    aut_action(a, 1, 3);
    aut_transition(a, (TransitionDef){0, 2, 'b', 'b'}, (DebugInfo){1, 5});
    aut_action(a, 2, 3);
    aut_transition(a, (TransitionDef){1, 3, 'd', 'd'}, (DebugInfo){2, 1});
    aut_action(a, 3, 4);
    aut_transition(a, (TransitionDef){2, 4, 'd', 'd'}, (DebugInfo){2, 5});
    aut_action(a, 4, 4);
    aut_optimize(a);
    aut_gen_dfa(a, w, false);
    aut_del(a);
    irwriter_end(w);
    irwriter_del(w);
    compat_close_memstream(f, &buf, &sz);
    opt = buf;
  }

  // Both must have action_id=3 and action_id=4
  assert(strstr(unopt, "i32 3, 1"));
  assert(strstr(unopt, "i32 4, 1"));
  assert(strstr(opt, "i32 3, 1"));
  assert(strstr(opt, "i32 4, 1"));

  free(unopt);
  free(opt);
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
  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "test.rules", ".");

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

TEST(test_compile_single) { _write_and_compile(_build_single, "single"); }

TEST(test_compile_range) { _write_and_compile(_build_range, "range"); }

TEST(test_compile_multi) { _write_and_compile(_build_multi, "multi"); }

TEST(test_compile_epsilon) { _write_and_compile(_build_epsilon, "epsilon"); }

TEST(test_compile_special) { _write_and_compile(_build_special_cp, "special"); }

TEST(test_compile_optimize) { _write_and_compile(_build_redundant, "optimize"); }

TEST(test_compile_debug) { _write_and_compile(_build_debug, "debug"); }

TEST(test_compile_debug_trap) { _write_and_compile(_build_debug_trap, "debug_trap"); }

TEST(test_compile_action_basic) { _write_and_compile(_build_action_basic, "action_basic"); }

TEST(test_compile_min_rule) { _write_and_compile(_build_min_rule, "min_rule"); }

TEST(test_compile_preserve) { _write_and_compile(_build_preserve, "preserve"); }

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
  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "test.rules", ".");

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
  RUN(test_single_transition);
  RUN(test_range_transition);
  RUN(test_multi_transitions);
  RUN(test_epsilon);
  RUN(test_special_codepoints);
  RUN(test_dead_state);
  RUN(test_action_smallest);
  RUN(test_action_basic);
  RUN(test_min_rule);
  RUN(test_debug_info);
  RUN(test_optimize);
  RUN(test_optimize_reduces_states);
  RUN(test_preserving_rule);
  RUN(test_optimize_preserves_action);
  RUN(test_compile_single);
  RUN(test_compile_range);
  RUN(test_compile_multi);
  RUN(test_compile_epsilon);
  RUN(test_compile_special);
  RUN(test_compile_optimize);
  RUN(test_compile_debug);
  RUN(test_debug_trap);
  RUN(test_compile_debug_trap);
  RUN(test_compile_action_basic);
  RUN(test_compile_min_rule);
  RUN(test_compile_preserve);
  printf("all ok\n");
  return 0;
}
