#include "../src/aut.h"
#include <assert.h>
#include <stdio.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

TEST(test_lifecycle) {
  Aut* a = aut_new("f", "test.rules");
  assert(a);
  aut_del(a);
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

int main(void) {
  printf("test_aut:\n");
  RUN(test_lifecycle);
  RUN(test_optimize_reduces_states);
  printf("all ok\n");
  return 0;
}
