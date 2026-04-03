#include "../src/symtab.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

TEST(test_init_free) {
  Symtab st = {0};
  symtab_init(&st);
  assert(symtab_count(&st) == 0);
  symtab_free(&st);
}

TEST(test_init_fn) {
  Symtab st = {0};
  symtab_init(&st);
  assert(symtab_count(&st) == 0);
  assert(symtab_find(&st, "x") == 0);
  symtab_free(&st);
}

TEST(test_intern_single) {
  Symtab st = {0};
  symtab_init(&st);
  int32_t id = symtab_intern(&st, "foo");
  assert(id == 1);
  assert(symtab_count(&st) == 1);
  assert(strcmp(symtab_get(&st, 1), "foo") == 0);
  symtab_free(&st);
}

TEST(test_intern_dedup) {
  Symtab st = {0};
  symtab_init(&st);
  int32_t id1 = symtab_intern(&st, "foo");
  int32_t id2 = symtab_intern(&st, "foo");
  assert(id1 == id2);
  assert(symtab_count(&st) == 1);
  symtab_free(&st);
}

TEST(test_intern_multiple) {
  Symtab st = {0};
  symtab_init(&st);
  int32_t a = symtab_intern(&st, "alpha");
  int32_t b = symtab_intern(&st, "beta");
  int32_t c = symtab_intern(&st, "gamma");
  assert(a == 1);
  assert(b == 2);
  assert(c == 3);
  assert(symtab_count(&st) == 3);
  assert(strcmp(symtab_get(&st, 1), "alpha") == 0);
  assert(strcmp(symtab_get(&st, 2), "beta") == 0);
  assert(strcmp(symtab_get(&st, 3), "gamma") == 0);
  symtab_free(&st);
}

TEST(test_find_present) {
  Symtab st = {0};
  symtab_init(&st);
  symtab_intern(&st, "x");
  symtab_intern(&st, "y");
  assert(symtab_find(&st, "x") == 1);
  assert(symtab_find(&st, "y") == 2);
  symtab_free(&st);
}

TEST(test_find_absent) {
  Symtab st = {0};
  symtab_init(&st);
  symtab_intern(&st, "x");
  assert(symtab_find(&st, "z") == 0);
  symtab_free(&st);
}

TEST(test_find_empty) {
  Symtab st = {0};
  symtab_init(&st);
  assert(symtab_find(&st, "anything") == 0);
  symtab_free(&st);
}

TEST(test_intern_empty_string) {
  Symtab st = {0};
  symtab_init(&st);
  int32_t id = symtab_intern(&st, "");
  assert(id == 1);
  assert(strcmp(symtab_get(&st, 1), "") == 0);
  int32_t id2 = symtab_intern(&st, "");
  assert(id2 == 1);
  symtab_free(&st);
}

TEST(test_prefixed_keys) {
  Symtab st = {0};
  symtab_init(&st);
  int32_t t1 = symtab_intern(&st, "tok:NUM");
  int32_t t2 = symtab_intern(&st, "scope:main");
  int32_t t3 = symtab_intern(&st, "tok:IDENT");
  int32_t t4 = symtab_intern(&st, "tok:NUM");
  assert(t1 == 1);
  assert(t2 == 2);
  assert(t3 == 3);
  assert(t4 == 1);
  assert(strcmp(symtab_get(&st, 2), "scope:main") == 0);
  symtab_free(&st);
}

TEST(test_many_entries) {
  Symtab st = {0};
  symtab_init(&st);
  char buf[32];
  for (int32_t i = 0; i < 200; i++) {
    snprintf(buf, sizeof(buf), "sym_%d", i);
    int32_t id = symtab_intern(&st, buf);
    assert(id == i + 1);
  }
  assert(symtab_count(&st) == 200);
  for (int32_t i = 0; i < 200; i++) {
    snprintf(buf, sizeof(buf), "sym_%d", i);
    assert(symtab_find(&st, buf) == i + 1);
    assert(strcmp(symtab_get(&st, i + 1), buf) == 0);
  }
  symtab_free(&st);
}

TEST(test_get_stable_after_growth) {
  Symtab st = {0};
  symtab_init(&st);
  symtab_intern(&st, "first");
  for (int32_t i = 0; i < 100; i++) {
    char buf[32];
    snprintf(buf, sizeof(buf), "grow_%d", i);
    symtab_intern(&st, buf);
  }
  assert(strcmp(symtab_get(&st, 1), "first") == 0);
  symtab_free(&st);
}

TEST(test_double_free) {
  Symtab st = {0};
  symtab_init(&st);
  symtab_intern(&st, "x");
  symtab_free(&st);
  symtab_free(&st);
}

int main(void) {
  printf("test_symtab:\n");
  RUN(test_init_free);
  RUN(test_init_fn);
  RUN(test_intern_single);
  RUN(test_intern_dedup);
  RUN(test_intern_multiple);
  RUN(test_find_present);
  RUN(test_find_absent);
  RUN(test_find_empty);
  RUN(test_intern_empty_string);
  RUN(test_prefixed_keys);
  RUN(test_many_entries);
  RUN(test_get_stable_after_growth);
  RUN(test_double_free);
  printf("all ok\n");
  return 0;
}
