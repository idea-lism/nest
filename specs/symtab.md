String-interning symbol table.

### Idea

A typical symbol table maps strings to integer IDs.
Naive approach: `char** symbols` (darray of `char*` pointers) where each entry is a separately `strdup`'d string.
This scatters small allocations across the heap and requires `char***` to propagate darray reallocations.

Instead, store all strings in a single flat `char*` buffer, separated by `'\0'`:

```
buf: "tok:NUM\0scope:main\0tok:IDENT\0"
      ^0       ^8          ^19        ^28
```

### Struct

```c
typedef struct {
  char* buf;          // darray of char
  int32_t* offsets;   // darray of int32_t (byte offsets into buf)
  int32_t start_num;  // first ID returned
} Symtab;
```

### Methods

```c
void     symtab_init(Symtab* st, int32_t start_num);      // validation: start_num must be >= 0
void     symtab_free(Symtab* st);
int32_t  symtab_intern(Symtab* st, const char* name);     // intern, returns id
int32_t  symtab_find(const Symtab* st, const char* name); // lookup, returns id or -1 (not found)
const char* symtab_get(const Symtab* st, int32_t id);     // get string by id
int32_t  symtab_count(const Symtab* st);                  // number of interned strings
```

- `symtab_init`: allocates both darrays (must be called before use).
- `symtab_free`: frees both darrays. Does not free the struct itself.
- `symtab_intern`: linear scan via offsets; on miss, appends string + `'\0'` to `buf`, appends new offset to `offsets`, returns ID (starting from `start_num`).
- `symtab_find`: like intern but returns -1 on miss.
- `symtab_get`: returns `&buf[offsets[id - start_num]]`. Caller must not modify the returned pointer.
- `symtab_count`: returns `darray_size(offsets)`.

### Usage

```c
Symtab st = {0};
symtab_init(&st, 0);
int32_t id1 = symtab_intern(&st, "foo");   // 0
int32_t id2 = symtab_intern(&st, "bar");   // 1
int32_t id3 = symtab_intern(&st, "foo");   // 0 (already interned)
assert(strcmp(symtab_get(&st, 0), "foo") == 0);
symtab_free(&st);
```
