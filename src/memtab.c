// specs/memtab.md
#include "memtab.h"
#include "xmalloc.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

MemoizeTable* memtab_new(size_t size, int64_t sizeof_col, int64_t slots_offset_in_i32, int64_t slots_size) {
  // sizeof_col must be multiple of 8 so the int32_t flexible array stays 8-byte aligned per column
  assert(sizeof_col % 8 == 0);
  // +1 for sentinel column
  size_t cols_bytesize = (size + 1) * (size_t)sizeof_col;
  MemoizeTable* mt = (MemoizeTable*)XMALLOC(sizeof(MemoizeTable) + cols_bytesize);
  mt->sizeof_col = sizeof_col;
  mt->slots_offset_in_i32 = slots_offset_in_i32;
  mt->slots_size = slots_size;
  mt->has_parse_error = 0;
  mt->size = (int32_t)size;
  // initial bits/slots are all 1 / -1
  memset(mt->t, -1, cols_bytesize);
  assert(((uintptr_t)mt->t & 7) == 0);
  return mt;
}

int32_t* memtab_col(MemoizeTable* mt, size_t col) { return (int32_t*)((uint8_t*)mt->t + col * (size_t)mt->sizeof_col); }

void memtab_del(MemoizeTable* mt) { XFREE(mt); }
