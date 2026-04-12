#pragma once

#include <stddef.h>
#include <stdint.h>

void* darray_new(uint32_t elem_size, size_t elem_count);
void* darray_grow(void* a, size_t new_elem_count);
// returns elem count (not bytesize)
size_t darray_size(void* a);
// free the heap, not the elem
void darray_del(void* a);

#define darray_push(arr, elem)                                                                                         \
  do {                                                                                                                 \
    size_t _n = darray_size(arr);                                                                                      \
    (arr) = darray_grow((arr), _n + 1);                                                                                \
    (arr)[_n] = (elem);                                                                                                \
  } while (0)

// assert 2 arrays not equal, & having the same elem_size
void* darray_concat(void* a, void* b);

void* darray_insert(void* a, size_t pos, const void* elem);
