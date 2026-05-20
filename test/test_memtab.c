#include "../src/memtab.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

TEST(test_new_del_empty) {
  MemoizeTable* mt = memtab_new(0, 8, 0, 1);
  assert(mt != NULL);
  assert(mt->sizeof_col == 8);
  assert(mt->slots_offset_in_i32 == 0);
  assert(mt->slots_size == 1);
  assert(mt->has_parse_error == 0);
  assert(mt->size == 0);
  // sentinel column still present
  int32_t* col0 = memtab_col(mt, 0);
  assert(col0[0] == -1);
  memtab_del(mt);
}

TEST(test_basic_layout) {
  // 3 tokens, sizeof_col = 16 bytes (4 i32 slots), slots_offset_in_i32 = 0
  MemoizeTable* mt = memtab_new(3, 16, 0, 4);
  assert(mt->size == 3);
  assert(mt->sizeof_col == 16);
  // 4 columns total (3 + 1 sentinel)
  for (size_t c = 0; c < 4; c++) {
    int32_t* col = memtab_col(mt, c);
    for (int i = 0; i < 4; i++) {
      assert(col[i] == -1);
    }
  }
  memtab_del(mt);
}

TEST(test_col_alignment) {
  MemoizeTable* mt = memtab_new(5, 24, 1, 5);
  for (size_t c = 0; c <= 5; c++) {
    int32_t* col = memtab_col(mt, c);
    assert(((uintptr_t)col & 7) == 0);
  }
  memtab_del(mt);
}

TEST(test_col_distinct) {
  MemoizeTable* mt = memtab_new(2, 8, 0, 2);
  int32_t* c0 = memtab_col(mt, 0);
  int32_t* c1 = memtab_col(mt, 1);
  int32_t* c2 = memtab_col(mt, 2); // sentinel
  assert((uint8_t*)c1 - (uint8_t*)c0 == 8);
  assert((uint8_t*)c2 - (uint8_t*)c1 == 8);
  memtab_del(mt);
}

TEST(test_writable) {
  MemoizeTable* mt = memtab_new(2, 16, 2, 2);
  int32_t* c0 = memtab_col(mt, 0);
  c0[2] = 7;
  c0[3] = 9;
  int32_t* again = memtab_col(mt, 0);
  assert(again[2] == 7);
  assert(again[3] == 9);
  // other columns untouched
  int32_t* c1 = memtab_col(mt, 1);
  assert(c1[2] == -1);
  assert(c1[3] == -1);
  memtab_del(mt);
}

TEST(test_slots_via_offset) {
  // sizeof_col = 16 bytes = 4 i32 cells; slots are last 2 cells (offset 2 in i32)
  MemoizeTable* mt = memtab_new(3, 16, 2, 2);
  for (size_t c = 0; c <= 3; c++) {
    int32_t* col = memtab_col(mt, c);
    int32_t* slots = col + mt->slots_offset_in_i32;
    for (int64_t s = 0; s < mt->slots_size; s++) {
      assert(slots[s] == -1);
    }
  }
  memtab_del(mt);
}

TEST(test_has_parse_error_field) {
  MemoizeTable* mt = memtab_new(1, 8, 0, 1);
  assert(mt->has_parse_error == 0);
  mt->has_parse_error = 1;
  assert(mt->has_parse_error == 1);
  memtab_del(mt);
}

int main(void) {
  printf("test_memtab:\n");
  RUN(test_new_del_empty);
  RUN(test_basic_layout);
  RUN(test_col_alignment);
  RUN(test_col_distinct);
  RUN(test_writable);
  RUN(test_slots_via_offset);
  RUN(test_has_parse_error_field);
  printf("all ok\n");
  return 0;
}
