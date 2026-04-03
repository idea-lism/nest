// specs/symtab.md
#include "symtab.h"
#include "darray.h"

#include <string.h>

void symtab_init(Symtab* st) {
  st->buf = darray_new(sizeof(char), 0);
  st->offsets = darray_new(sizeof(int32_t), 0);
}

void symtab_free(Symtab* st) {
  darray_del(st->buf);
  darray_del(st->offsets);
  st->buf = NULL;
  st->offsets = NULL;
}

int32_t symtab_find(const Symtab* st, const char* name) {
  int32_t n = (int32_t)darray_size(st->offsets);
  for (int32_t i = 0; i < n; i++) {
    if (strcmp(st->buf + st->offsets[i], name) == 0) {
      return i + 1;
    }
  }
  return 0;
}

int32_t symtab_intern(Symtab* st, const char* name) {
  int32_t id = symtab_find(st, name);
  if (id) {
    return id;
  }
  int32_t off = (int32_t)darray_size(st->buf);
  darray_push(st->offsets, off);
  size_t len = strlen(name);
  st->buf = darray_grow(st->buf, darray_size(st->buf) + len + 1);
  memcpy(st->buf + off, name, len + 1);
  return (int32_t)darray_size(st->offsets);
}

const char* symtab_get(const Symtab* st, int32_t id) { return st->buf + st->offsets[id - 1]; }

int32_t symtab_count(const Symtab* st) { return (int32_t)darray_size(st->offsets); }
