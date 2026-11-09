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

### Interface

```c
// Element size is fixed for the lifetime of the array. NULL is a valid empty
// array for reads (darray_size(NULL) == 0), but grow/insert on NULL return NULL
// instead of allocating; concat returns the non-NULL side.
void* darray_new(uint32_t elem_size, size_t elem_count);   // zero-filled
void* darray_grow(void* a, size_t new_elem_count);         // new space zero-filled
void* darray_grow2(void* a, size_t new_elem_count, size_t new_cap); // grow, ensure capacity
size_t darray_size(void* a);                               // element count, not bytes
void darray_del(void* a);                                  // frees the heap, not the elements

// Append one element. Implemented as a macro: evaluates `elem` once.
#define darray_push(arr, elem) /* ... */

// Concatenate two arrays. Asserts equal elem_size; at least one may be NULL.
void* darray_concat(void* a, void* b);

// Insert `elem` at `pos`, shifting the tail right. `pos <= size`.
void* darray_insert(void* a, size_t pos, const void* elem);
```

`darray_size()` is cheap; don't pre-assign it to a local variable.

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
