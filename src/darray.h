#pragma once

#include <stddef.h>
#include <stdint.h>

void* darray_new_(uint32_t elem_size, size_t elem_count);
void* darray_grow_(void* a, size_t new_elem_count);
// returns elem count (not bytesize)
size_t darray_size(void* a);
// free the heap, not the elem
void darray_del_(void* a);

// assert 2 arrays not equal, & having the same elem_size
void* darray_concat_(void* a, void* b);

void* darray_insert_(void* a, size_t pos, const void* elem);

#ifdef XMALLOC_TRACE

void* darray_new_traced(uint32_t elem_size, size_t elem_count, const char* caller, int line);
void* darray_grow_traced(void* a, size_t new_elem_count, const char* caller, int line);
void darray_del_traced(void* a, const char* caller, int line);
void* darray_concat_traced(void* a, void* b, const char* caller, int line);
void* darray_insert_traced(void* a, size_t pos, const void* elem, const char* caller, int line);

#define darray_new(elem_size, elem_count) darray_new_traced(elem_size, elem_count, __FUNCTION__, __LINE__)
#define darray_grow(arr, new_elem_count) darray_grow_traced(arr, new_elem_count, __FUNCTION__, __LINE__)
#define darray_del(arr) darray_del_traced(arr, __FUNCTION__, __LINE__)
#define darray_concat(a, b) darray_concat_traced(a, b, __FUNCTION__, __LINE__)
#define darray_insert(a, pos, elem) darray_insert_traced(a, pos, elem, __FUNCTION__, __LINE__)

#else

#define darray_new darray_new_
#define darray_grow darray_grow_
#define darray_del darray_del_
#define darray_concat darray_concat_
#define darray_insert darray_insert_

#endif

#define darray_push(arr, elem)                                                                                         \
  do {                                                                                                                 \
    size_t _n = darray_size(arr);                                                                                      \
    (arr) = darray_grow((arr), _n + 1);                                                                                \
    (arr)[_n] = (elem);                                                                                                \
  } while (0)
