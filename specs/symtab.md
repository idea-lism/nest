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

A separate `int32_t* offsets` darray maps 0-based index to byte offset in `buf`.
IDs returned to callers are **1-based** (0 means "not found").

### Struct

```c
typedef struct {
  char* buf;         // darray of char
  int32_t* offsets;  // darray of int32_t (byte offsets into buf)
} Symtab;
```

### Methods

```c
void     symtab_init(Symtab* st);
void     symtab_free(Symtab* st);
int32_t  symtab_intern(Symtab* st, const char* name);          // intern, returns 1-based id
int32_t  symtab_find(const Symtab* st, const char* name);      // lookup, returns 1-based id or 0
const char* symtab_get(const Symtab* st, int32_t id);           // get string by 1-based id
int32_t  symtab_count(const Symtab* st);                        // number of interned strings
```

- `symtab_init`: sets both darrays to NULL (zero-initialized struct also works).
- `symtab_free`: frees both darrays. Does not free the struct itself.
- `symtab_intern`: linear scan via offsets; on miss, appends string + `'\0'` to `buf`, appends new offset to `offsets`, returns new 1-based ID.
- `symtab_find`: like intern but returns 0 on miss.
- `symtab_get`: returns `&buf[offsets[id - 1]]`. Caller must not modify the returned pointer.
- `symtab_count`: returns `darray_size(offsets)`.

### Usage

```c
Symtab st = {0};
int32_t id1 = symtab_intern(&st, "foo");   // 1
int32_t id2 = symtab_intern(&st, "bar");   // 2
int32_t id3 = symtab_intern(&st, "foo");   // 1 (already interned)
assert(strcmp(symtab_get(&st, 1), "foo") == 0);
symtab_free(&st);
```
