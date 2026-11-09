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

### Interface

```c
// IDs are dense: start_num, start_num + 1, ...
typedef struct {
  char* buf;         // darray of char: all strings concatenated, '\0'-separated
  int32_t* offsets;  // darray of int32_t: byte offset of each string in buf
  int32_t start_num; // first ID returned; must be >= 0
} Symtab;

// Allocate both darrays. Must be called before use.
void symtab_init(Symtab* st, int32_t start_num);

// Free both darrays. Does not free the Symtab struct itself.
void symtab_free(Symtab* st);

// Intern `name`, returning its ID. Linear scan via offsets; on miss appends
// name + '\0' to buf and a new offset, then returns the next ID.
int32_t symtab_intern(Symtab* st, const char* name);
int32_t symtab_intern_f(Symtab* st, const char* fmt_str, ...);

// Lookup only. Returns -1 when not found.
int32_t symtab_find(const Symtab* st, const char* name);
int32_t symtab_find_f(const Symtab* st, const char* fmt_str, ...);

// Returns &buf[offsets[id - start_num]]. Caller must not modify the string.
const char* symtab_get(const Symtab* st, int32_t id);

// Number of interned strings: darray_size(offsets).
size_t symtab_count(const Symtab* st);
```

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
