#include "../src/darray.h"
#include "../src/re.h"
#include <assert.h>
#include <stdio.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

// --- Lifecycle ---

TEST(test_lifecycle) {
  Aut* a = aut_new();
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
  printf("all ok\n");
  return 0;
}
