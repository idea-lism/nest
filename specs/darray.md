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
size_t darray_size(void* a); // returns elem size
void darray_del(void* a); // free the heap, not the elem
```

### Usage

`darray_size()` is very light-weighted, don't pre-assign it to local variables to make code bug-prone.
