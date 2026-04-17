A string library similar to sds

- Use simdutf-like algorithm to validate input -- we only accept valid UTF-8 input for now.
- The modified algorithm is: as validation goes on, the string is indexed in a bitset (`marks` field below), each bit represents whether a byte starts a codepoint.

Fat-pointer string: 

```
struct Heap {
  int32_t size;
  char[size] data;
  char nul = '\0';
  char[(size + 7)/8] marks;
};
```

And we pass `data` pointer around for ease of use.

Resulting code:

- src/ustr.h
  - Basic API `ustr_new(size_t sz, char* data)`, `ustr_del(char* s)`
    - when `new` returns `NULL` indicating an error, provide a helper procedure: `ErrType ustr_find_error(size_t, char* data, size_t* pos)` to get the error position and problem type.
  - Read from file `ustr_from_file(FILE* file)`, which reduces malloc to only once.
  - Codepoint iterator `ustr_iter_init(it, ustr, char_offset), ustr_iter_next` by scanning marks
    - iterator can start from middle of string by an offset
    - if the init offset is out of range, `assert(false)`
  - Slicing `ustr_slice(char* s, int32_t cp_start, int32_t cp_end)`
    - slicing is optimized with popcnt
- src/ustr.c
- src/ustr_neon.c, src/ustr_avx.c
  - SIMD accelerated routine for `ustr_new`

Extra iterator interface:

- `ustr_iter_seek(it, int32_t cp_offset)`: seek iterator to cp_offset.
  - for faster backtracking, if `cp_offset + 64 > it->cp_idx`, scan starts from cp_offset, else start from 0 (re-init).

Also add tests in:

- test/test_ustr.c
