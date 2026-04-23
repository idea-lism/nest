#include "darray.h"
#include "xmalloc.h"

#include <assert.h>
#include <string.h>

typedef struct {
  uint32_t byte_cap;
  uint32_t elem_size;
  size_t elem_count;
} DarrayHeader;

static DarrayHeader* _header(void* a) { return (DarrayHeader*)a - 1; }

void* darray_new_(uint32_t elem_size, size_t elem_count) {
  uint32_t byte_cap = (uint32_t)(elem_count * elem_size);
  if (byte_cap < elem_size * 4) {
    byte_cap = elem_size * 4;
  }
  DarrayHeader* h = XMALLOC(sizeof(DarrayHeader) + byte_cap);
  h->byte_cap = byte_cap;
  h->elem_size = elem_size;
  h->elem_count = elem_count;
  void* data = h + 1;
  memset(data, 0, byte_cap);
  return data;
}

void* darray_grow_(void* a, size_t new_elem_count) { return darray_grow2_(a, new_elem_count, new_elem_count); }

void* darray_grow2_(void* a, size_t new_elem_count, size_t new_cap) {
  if (!a) {
    return NULL;
  }
  DarrayHeader* h = _header(a);
  size_t old_elem_count = h->elem_count;
  h->elem_count = new_elem_count;
  size_t cap = new_cap > new_elem_count ? new_cap : new_elem_count;
  uint32_t needed = (uint32_t)(cap * h->elem_size);
  if (needed <= h->byte_cap) {
    if (cap > old_elem_count) {
      memset((char*)a + old_elem_count * h->elem_size, 0, (cap - old_elem_count) * h->elem_size);
    }
    return a;
  }
  uint32_t new_byte_cap = h->byte_cap;
  while (new_byte_cap < needed) {
    new_byte_cap *= 2;
  }
  uint32_t old_cap = h->byte_cap;
  h = XREALLOC(h, sizeof(DarrayHeader) + new_byte_cap);
  h->byte_cap = new_byte_cap;
  memset((char*)(h + 1) + old_cap, 0, new_byte_cap - old_cap);
  return h + 1;
}

size_t darray_size(void* a) {
  if (!a) {
    return 0;
  }
  return _header(a)->elem_count;
}

void darray_del_(void* a) {
  if (!a) {
    return;
  }
  XFREE(_header(a));
}

void* darray_concat_(void* a, void* b) {
  assert(a != b);
  if (!a || !b) {
    return a;
  }
  DarrayHeader* hb = _header(b);
  size_t bn = hb->elem_count;
  if (bn == 0) {
    return a;
  }
  uint32_t es = _header(a)->elem_size;
  assert(es == hb->elem_size);
  size_t an = _header(a)->elem_count;
  a = darray_grow_(a, an + bn);
  memcpy((char*)a + an * es, b, bn * es);
  return a;
}

void* darray_insert_(void* a, size_t pos, const void* elem) {
  if (!a) {
    return a;
  }
  size_t n = _header(a)->elem_count;
  uint32_t es = _header(a)->elem_size;
  a = darray_grow_(a, n + 1);
  char* p = (char*)a + pos * es;
  memmove(p + es, p, (n - pos) * es);
  memcpy(p, elem, es);
  return a;
}

#ifdef XMALLOC_TRACE

void* darray_new_traced(uint32_t elem_size, size_t elem_count, const char* caller, int line) {
  uint32_t byte_cap = (uint32_t)(elem_count * elem_size);
  if (byte_cap < elem_size * 4) {
    byte_cap = elem_size * 4;
  }
  DarrayHeader* h = xmalloc_traced(sizeof(DarrayHeader) + byte_cap, caller, line);
  h->byte_cap = byte_cap;
  h->elem_size = elem_size;
  h->elem_count = elem_count;
  void* data = h + 1;
  memset(data, 0, byte_cap);
  return data;
}

void* darray_grow_traced(void* a, size_t new_elem_count, const char* caller, int line) {
  return darray_grow2_traced(a, new_elem_count, new_elem_count, caller, line);
}

void* darray_grow2_traced(void* a, size_t new_elem_count, size_t new_cap, const char* caller, int line) {
  if (!a) {
    return NULL;
  }
  DarrayHeader* h = _header(a);
  size_t old_elem_count = h->elem_count;
  h->elem_count = new_elem_count;
  size_t cap = new_cap > new_elem_count ? new_cap : new_elem_count;
  uint32_t needed = (uint32_t)(cap * h->elem_size);
  if (needed <= h->byte_cap) {
    if (cap > old_elem_count) {
      memset((char*)a + old_elem_count * h->elem_size, 0, (cap - old_elem_count) * h->elem_size);
    }
    return a;
  }
  uint32_t new_byte_cap = h->byte_cap;
  while (new_byte_cap < needed) {
    new_byte_cap *= 2;
  }
  uint32_t old_cap = h->byte_cap;
  h = xrealloc_traced(h, sizeof(DarrayHeader) + new_byte_cap, caller, line);
  h->byte_cap = new_byte_cap;
  memset((char*)(h + 1) + old_cap, 0, new_byte_cap - old_cap);
  return h + 1;
}

void darray_del_traced(void* a, const char* caller, int line) {
  if (!a) {
    return;
  }
  xfree_traced(_header(a), caller, line);
}

void* darray_concat_traced(void* a, void* b, const char* caller, int line) {
  assert(a != b);
  if (!a || !b) {
    return a;
  }
  DarrayHeader* hb = _header(b);
  size_t bn = hb->elem_count;
  if (bn == 0) {
    return a;
  }
  uint32_t es = _header(a)->elem_size;
  assert(es == hb->elem_size);
  size_t an = _header(a)->elem_count;
  a = darray_grow_traced(a, an + bn, caller, line);
  memcpy((char*)a + an * es, b, bn * es);
  return a;
}

void* darray_insert_traced(void* a, size_t pos, const void* elem, const char* caller, int line) {
  if (!a) {
    return a;
  }
  size_t n = _header(a)->elem_count;
  uint32_t es = _header(a)->elem_size;
  a = darray_grow_traced(a, n + 1, caller, line);
  char* p = (char*)a + pos * es;
  memmove(p + es, p, (n - pos) * es);
  memcpy(p, elem, es);
  return a;
}

#endif
