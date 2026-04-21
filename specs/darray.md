Dynamic array util.

### Idea

A typical dynamic array is:

```c
struct A {
  Elem* elems;
  size_t size;
  size_t cap;
};
```

With fat pointer trick, we can put size / cap before the elems start, on heap it is:

```c
struct Heap {
  uint32_t byte_cap;
  uint32_t elem_size;
  size_t elem_count;
  Elem elems[]; // after alloc / realloc, we return the start address of elems
}
```

Then the usage site is simply:

```c
struct A {
  Elem* elems;
};
```

### Methods

```c
void* darray_new(uint32_t elem_size, size_t elem_count);
void* darray_grow(void* a, size_t new_elem_count); // returns realloc-ed darray
// returns elem count (not bytesize)
size_t darray_size(void* a);
// free the heap, not the elem
void darray_del(void* a);
```

### Usage

`darray_size()` is very light-weighted, don't pre-assign it to local variables to make code bug-prone.

### Leak Detection

darray is used a lot. So for more helpful [leak detection](xmalloc.md), we create macros to forward caller info for the APIs. For example, `darray_new` should be defined as:

```c
#ifdef XMALLOC_TRACE
#define darray_new(elem_size, elem_count) darray_new_traced(elem_size, elem_count, __FUNCTION__, __LINE__)
#else
#define darray_new darray_new_
#endif
```

And the traced version directly uses `xmalloc_traced` instead.
