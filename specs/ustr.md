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
  - Basic API `ustr_new(size_t sz, char* data, int32_t* err)`, `ustr_del(char* s)`
  - Codepoint iterator by scanning marks, which also trackes line and col.
    - iterator can start from middle of string by an offset
  - Slicing `ustr_slice(char* s, int32_t cp_start, int32_t cp_end)`
    - slicing is optimized with popcnt
  - Concat `ustr_cat(char* a, char* b)`
- src/ustr.c
- src/ustr_arm64.c
  - SIMD accelerated routine for `ustr_new`

Also add tests in:

- test/test_ustr.c
