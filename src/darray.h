#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

void* darray_new(uint32_t elem_size, size_t elem_count);
void* darray_grow(void* a, size_t new_elem_count);
size_t darray_size(void* a);
void darray_del(void* a);

#define darray_push(arr, elem)                                                                                         \
  do {                                                                                                                 \
    size_t _n = darray_size(arr);                                                                                      \
    (arr) = darray_grow((arr), _n + 1);                                                                                \
    (arr)[_n] = (elem);                                                                                                \
  } while (0)

#define darray_concat(dst, src, count)                                                                                 \
  do {                                                                                                                 \
    size_t _n = darray_size(dst);                                                                                      \
    size_t _c = (size_t)(count);                                                                                       \
    (dst) = darray_grow((dst), _n + _c);                                                                               \
    memcpy(&(dst)[_n], (src), _c * sizeof(*(dst)));                                                                    \
  } while (0)

#define darray_insert(arr, pos, elem)                                                                                  \
  do {                                                                                                                 \
    size_t _n = darray_size(arr);                                                                                      \
    (arr) = darray_grow((arr), _n + 1);                                                                                \
    memmove(&(arr)[(pos) + 1], &(arr)[(pos)], (_n - (size_t)(pos)) * sizeof(*(arr)));                                  \
    (arr)[(pos)] = (elem);                                                                                             \
  } while (0)
