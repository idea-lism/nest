#include "darray.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  uint32_t byte_cap;
  uint32_t elem_size;
  size_t elem_count;
} DarrayHeader;

static DarrayHeader* _header(void* a) { return (DarrayHeader*)a - 1; }

void* darray_new(uint32_t elem_size, size_t elem_count) {
  uint32_t byte_cap = (uint32_t)(elem_count * elem_size);
  if (byte_cap < elem_size * 4) {
    byte_cap = elem_size * 4;
  }
  DarrayHeader* h = malloc(sizeof(DarrayHeader) + byte_cap);
  h->byte_cap = byte_cap;
  h->elem_size = elem_size;
  h->elem_count = elem_count;
  void* data = h + 1;
  memset(data, 0, byte_cap);
  return data;
}

void* darray_grow(void* a, size_t new_elem_count) {
  if (!a) {
    return NULL;
  }
  DarrayHeader* h = _header(a);
  h->elem_count = new_elem_count;
  uint32_t needed = (uint32_t)(new_elem_count * h->elem_size);
  if (needed <= h->byte_cap) {
    return a;
  }
  uint32_t new_cap = h->byte_cap;
  while (new_cap < needed) {
    new_cap *= 2;
  }
  uint32_t old_cap = h->byte_cap;
  h = realloc(h, sizeof(DarrayHeader) + new_cap);
  h->byte_cap = new_cap;
  memset((char*)(h + 1) + old_cap, 0, new_cap - old_cap);
  return h + 1;
}

size_t darray_size(void* a) {
  if (!a) {
    return 0;
  }
  return _header(a)->elem_count;
}

void darray_del(void* a) {
  if (!a) {
    return;
  }
  free(_header(a));
}
