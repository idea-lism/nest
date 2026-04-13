// specs/symtab.md
#include "symtab.h"
#include "darray.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void symtab_init(Symtab* st, int32_t start_num) {
  assert(start_num >= 0);
  st->buf = darray_new(sizeof(char), 0);
  st->offsets = darray_new(sizeof(int32_t), 0);
  st->start_num = start_num;
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
      return i + st->start_num;
    }
  }
  return -1;
}

int32_t symtab_find_f(const Symtab* st, const char* fmt_str, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt_str);
  vsnprintf(buf, sizeof(buf), fmt_str, ap);
  va_end(ap);
  return symtab_find(st, buf);
}

int32_t symtab_intern(Symtab* st, const char* name) {
  int32_t id = symtab_find(st, name);
  if (id != -1) {
    return id;
  }
  int32_t off = (int32_t)darray_size(st->buf);
  darray_push(st->offsets, off);
  size_t len = strlen(name);
  st->buf = darray_grow(st->buf, darray_size(st->buf) + len + 1);
  memcpy(st->buf + off, name, len + 1);
  return (int32_t)darray_size(st->offsets) - 1 + st->start_num;
}

int32_t symtab_intern_f(Symtab* st, const char* fmt_str, ...) {
  va_list ap;
  va_start(ap, fmt_str);
  int len = vsnprintf(NULL, 0, fmt_str, ap);
  va_end(ap);

  int32_t off = (int32_t)darray_size(st->buf);
  st->buf = darray_grow(st->buf, darray_size(st->buf) + len + 1);

  va_start(ap, fmt_str);
  vsnprintf(st->buf + off, len + 1, fmt_str, ap);
  va_end(ap);

  int32_t id = symtab_find(st, st->buf + off);
  if (id != -1) {
    st->buf = darray_grow(st->buf, (size_t)off);
    return id;
  }
  darray_push(st->offsets, off);
  return (int32_t)darray_size(st->offsets) - 1 + st->start_num;
}

const char* symtab_get(const Symtab* st, int32_t id) { return st->buf + st->offsets[id - st->start_num]; }

int32_t symtab_count(const Symtab* st) { return (int32_t)darray_size(st->offsets); }
