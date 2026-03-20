#include "../src/aut.h"
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

static char* gen_ir(void (*fn)(Aut*, IrWriter*)) {
  char* buf = NULL;
  size_t sz = 0;
  FILE* f = open_memstream(&buf, &sz);
  assert(f);
  IrWriter* w = irwriter_new(f, TARGET);
  irwriter_start(w, "test.rules", ".");

  Aut* a = aut_new("match", "test.rules");
  fn(a, w);
  aut_del(a);

  irwriter_end(w);
  irwriter_del(w);
  fclose(f);
  return buf;
}

// --- Basic: single transition ---

static void build_single(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, 'A', 'A', 1}, (DebugInfo){1, 1});
  aut_gen_dfa(a, w, false);
}

TEST(test_single_transition) {
  char* out = gen_ir(build_single);
  assert(strstr(out, "define {i32, i32} @match(i32 %state, i32 %cp)"));
  assert(strstr(out, "switch i32 %state, label %dead"));
  // State 0 should have a switch on cp with case 65 ('A')
  assert(strstr(out, "i32 65, label %s0_t0"));
  // Match block returns {1, 1}
  assert(strstr(out, "s0_t0:"));
  free(out);
}

// --- Range transition ---

static void build_range(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, 'A', 'Z', 1}, (DebugInfo){1, 1});
  aut_gen_dfa(a, w, false);
}

TEST(test_range_transition) {
  char* out = gen_ir(build_range);
  // Should use range check: icmp sge ... 65, icmp sle ... 90
  assert(strstr(out, "icmp sge i32 %cp, 65"));
  assert(strstr(out, "icmp sle i32 %cp, 90"));
  free(out);
}

// --- Multiple transitions from one state ---

static void build_multi(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, 'A', 'Z', 1}, (DebugInfo){1, 1});
  aut_transition(a, (TransitionDef){0, 2, 'a', 'z', 2}, (DebugInfo){2, 1});
  aut_gen_dfa(a, w, false);
}

TEST(test_multi_transitions) {
  char* out = gen_ir(build_multi);
  // Two range checks
  assert(strstr(out, "icmp sge i32 %cp, 65"));
  assert(strstr(out, "icmp sle i32 %cp, 90"));
  assert(strstr(out, "icmp sge i32 %cp, 97"));
  assert(strstr(out, "icmp sle i32 %cp, 122"));
  free(out);
}

// --- Epsilon transitions ---

static void build_epsilon(Aut* a, IrWriter* w) {
  // State 0 --eps--> state 1, state 1 --'x'--> state 2 with action 1
  aut_epsilon(a, 0, 1);
  aut_transition(a, (TransitionDef){1, 2, 'x', 'x', 1}, (DebugInfo){1, 1});
  aut_gen_dfa(a, w, false);
}

TEST(test_epsilon) {
  char* out = gen_ir(build_epsilon);
  // After determinization, DFA state 0 = {NFA 0, NFA 1} (eps closure).
  // So from DFA state 0, input 'x' should go somewhere with action 1.
  assert(strstr(out, "i32 120")); // 'x' = 120
  free(out);
}

// --- Special codepoints ---

static void build_special_cp(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, -1, -1, 0}, (DebugInfo){1, 1}); // BOF
  aut_transition(a, (TransitionDef){1, 2, 'a', 'z', 1}, (DebugInfo){1, 5});
  aut_transition(a, (TransitionDef){2, 3, -2, -2, 2}, (DebugInfo){1, 10}); // EOF
  aut_gen_dfa(a, w, false);
}

TEST(test_special_codepoints) {
  char* out = gen_ir(build_special_cp);
  // BOF = -1, EOF = -2 treated as normal codepoints
  assert(strstr(out, "-1"));
  assert(strstr(out, "-2"));
  free(out);
}

// --- Dead state ---

TEST(test_dead_state) {
  char* out = gen_ir(build_single);
  // Dead state should return {state, -2}
  assert(strstr(out, "dead:"));
  free(out);
}

// --- Action ID: smallest returned ---

static void build_action_smallest(Aut* a, IrWriter* w) {
  // Two transitions from state 0 on overlapping range, different actions
  // action 5 on [A-Z], action 3 on [M-M]
  aut_transition(a, (TransitionDef){0, 1, 'A', 'Z', 5}, (DebugInfo){1, 1});
  aut_transition(a, (TransitionDef){0, 1, 'M', 'M', 3}, (DebugInfo){1, 5});
  aut_gen_dfa(a, w, false);
}

TEST(test_action_smallest) {
  char* out = gen_ir(build_action_smallest);
  // For 'M' (77), we expect the NFA has both transitions matching.
  // The determinized transition for the sub-interval covering 'M' should pick action_id=3 (smallest).
  // Check that 3 appears as an action_id insertvalue
  assert(strstr(out, "i32 3, 1"));
  free(out);
}

// --- Debug info ---

static void build_debug(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, 'A', 'A', 1}, (DebugInfo){10, 5});
  aut_gen_dfa(a, w, false);
}

TEST(test_debug_info) {
  char* out = gen_ir(build_debug);
  assert(strstr(out, "DILocation(line: 10, column: 5"));
  assert(strstr(out, "DISubprogram(name: \"match\""));
  free(out);
}

// --- Debug trap on dead state ---

static void build_debug_trap(Aut* a, IrWriter* w) {
  aut_transition(a, (TransitionDef){0, 1, 'A', 'A', 1}, (DebugInfo){1, 1});
  aut_gen_dfa(a, w, true);
}

TEST(test_debug_trap) {
  char* out = gen_ir(build_debug_trap);
  assert(strstr(out, "declare void @llvm.debugtrap()"));
  assert(strstr(out, "call void @llvm.debugtrap()"));
  free(out);
}

// --- Optimize (Brzozowski) ---

static void build_redundant(Aut* a, IrWriter* w) {
  // Create redundant NFA: states 0,1,2 where 1 and 2 behave identically
  // 0 --'a'--> 1, 0 --'b'--> 2
  // 1 --'c'--> 3 (action 1), 2 --'c'--> 3 (action 1)
  aut_transition(a, (TransitionDef){0, 1, 'a', 'a', 0}, (DebugInfo){1, 1});
  aut_transition(a, (TransitionDef){0, 2, 'b', 'b', 0}, (DebugInfo){1, 5});
  aut_transition(a, (TransitionDef){1, 3, 'c', 'c', 1}, (DebugInfo){2, 1});
  aut_transition(a, (TransitionDef){2, 3, 'c', 'c', 1}, (DebugInfo){2, 5});
  aut_optimize(a);
  aut_gen_dfa(a, w, false);
}

TEST(test_optimize) {
  char* out = gen_ir(build_redundant);
  // After Brzozowski, states 1 and 2 should be merged.
  // The IR should still be valid and contain the expected structure.
  assert(strstr(out, "define {i32, i32} @match"));
  assert(strstr(out, "switch i32 %state, label %dead"));
  free(out);
}

// --- Clang compilation ---

static void write_and_compile(void (*fn)(Aut*, IrWriter*), const char* test_name) {
  char ll_path[128], obj_path[128];
  snprintf(ll_path, sizeof(ll_path), "/tmp/test_aut_%s.ll", test_name);
  snprintf(obj_path, sizeof(obj_path), "/tmp/test_aut_%s.o", test_name);

  FILE* f = fopen(ll_path, "w");
  assert(f);
  IrWriter* w = irwriter_new(f, TARGET);
  irwriter_start(w, "test.rules", ".");

  Aut* a = aut_new("match", "test.rules");
  fn(a, w);
  aut_del(a);

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

TEST(test_compile_single) { write_and_compile(build_single, "single"); }

TEST(test_compile_range) { write_and_compile(build_range, "range"); }

TEST(test_compile_multi) { write_and_compile(build_multi, "multi"); }

TEST(test_compile_epsilon) { write_and_compile(build_epsilon, "epsilon"); }

TEST(test_compile_special) { write_and_compile(build_special_cp, "special"); }

TEST(test_compile_optimize) { write_and_compile(build_redundant, "optimize"); }

TEST(test_compile_debug) { write_and_compile(build_debug, "debug"); }

TEST(test_compile_debug_trap) { write_and_compile(build_debug_trap, "debug_trap"); }

// --- Lifecycle ---

TEST(test_lifecycle) {
  Aut* a = aut_new("f", "test.rules");
  assert(a);
  aut_del(a);
}

TEST(test_empty_aut) {
  char* buf = NULL;
  size_t sz = 0;
  FILE* f = open_memstream(&buf, &sz);
  assert(f);
  IrWriter* w = irwriter_new(f, TARGET);
  irwriter_start(w, "test.rules", ".");

  Aut* a = aut_new("empty", "test.rules");
  aut_gen_dfa(a, w, false);
  aut_del(a);

  irwriter_end(w);
  irwriter_del(w);
  fclose(f);

  assert(strstr(buf, "define {i32, i32} @empty"));
  assert(strstr(buf, "dead:"));
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
  RUN(test_debug_info);
  RUN(test_optimize);
  RUN(test_compile_single);
  RUN(test_compile_range);
  RUN(test_compile_multi);
  RUN(test_compile_epsilon);
  RUN(test_compile_special);
  RUN(test_compile_optimize);
  RUN(test_compile_debug);
  RUN(test_debug_trap);
  RUN(test_compile_debug_trap);
  printf("all ok\n");
  return 0;
}
