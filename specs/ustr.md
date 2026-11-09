A string library similar to sds.

- Use a simdutf-like algorithm to validate input -- we only accept valid UTF-8 input for now.
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

And we pass the `data` pointer around for ease of use.

### Interface

```c
// --- Creation / destruction ---

// Validate `data` and copy it into a new ustr. Returns NULL on invalid UTF-8.
char* ustr_new(size_t sz, const char* data);

// Read the whole file. Allocates once; the result is empty for an empty file.
// Returns NULL on read / validation failure.
char* ustr_from_file(FILE* file);

void ustr_del(char* s);

// --- Validation diagnostics ---

typedef enum {
  USTR_ERR_NONE,
  USTR_ERR_INVALID,
  USTR_ERR_TRUNCATED,
} UstrErr;

// When ustr_new returns NULL, locate the offending byte and error kind.
UstrErr ustr_find_error(size_t sz, const char* data, size_t* pos);

// --- Codepoint-level views ---

// Number of codepoints / bytes in the ustr.
int32_t ustr_size(const char* s);
int32_t ustr_bytesize(const char* s);

// Codepoint at index, or -1 if out of range.
int32_t ustr_cp_at(const char* s, int32_t cp_offset);

// Copy a codepoint range into a new ustr. Uses popcnt for the fast path.
char* ustr_slice(const char* s, int32_t cp_start, int32_t cp_end);

// Byte slice of a codepoint range. Does not copy.
typedef struct {
  const char* s;
  int32_t size;
} UstrByteSlice;
UstrByteSlice ustr_slice_bytes(const char* s, int32_t cp_start, int32_t cp_end);

// One codepoint copied out as UTF-8 bytes.
typedef struct {
  char buf[4];
} UstrCpBuf;
UstrCpBuf ustr_slice_cp(const char* s, int32_t cp_offset);

// Encode `cp` as UTF-8 into `out`; returns the byte count.
int32_t ustr_encode_utf8(char* out, int32_t cp);

// --- Iterator ---

// Works on the codepoint level, not the byte level.
typedef struct {
  const char* s;        // the ustr start
  const uint8_t* marks; // point to ustr's bit marks
  int32_t bytesize;     // ustr's byte size
  int32_t byte_index;
  int32_t cp_index;
} UstrIter;

// Start at a codepoint offset; asserts when out of range.
void ustr_iter_init(UstrIter* it, const char* s, int32_t cp_offset);
// Seek to a codepoint offset. A backward seek within 64 codepoints scans the
// marks backwards; any other seek re-inits from byte 0.
void ustr_iter_seek(UstrIter* it, int32_t cp_offset);
// Next codepoint, or -1 at end.
int32_t ustr_iter_next(UstrIter* it);
```

Tests live in `test/test_ustr.c`: invalid UTF-8 at every boundary (overlong, surrogate, truncated 2/3/4-byte, >0x10FFFF, F5/FE/FF), slicing, iterator seek, and SIMD/scalar consistency.
