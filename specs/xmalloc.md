# XMALLOC

`xmalloc.h` defines XMALLOC/XFREE macro that, when `calloc`/`malloc`/`realloc` fails (OOM), `abort()` with call-site message: `__FUNCTION__:__LINE__`

when defined macro `XMALLOC_TRACE` the macros routes to traced versions: `calloc_traced`/`xmalloc_traced`/`xrealloc_traced`/`xfree_traced`.

when defined macro `XMALLOC_TRACE`, `xmalloc.c` has an initializer that defines `atexit()` hook at `__attribute__((constructor))` to check if there are leaks. Leak reporting will include the call site info too.

Leak tracking data structure (an open-addressing hash table on allocated ptr, choose a simple hashing function & probe function, expand table when there's no more probe):

```c
typedef struct {
  void* ptr;
  // call site info
  const char* caller;
  int line;
} Pointer;

typedef struct {
  Pointer p;
  int probe_token; // 0:slot empty, 1:hash, 2:probe1(hash), 3:probe2(hash), 4:probe3(hash), if still no empty slot at 4, expand the hash table
} PointerBucket;

typedef struct {
  size_t bucket_cap;
  PointerBucket* pointer_buckets;
} PointerTracker;
```

# LLVM IR Adaption

For calloc/malloc/realloc/free in generated LLVM IR, also implement a similar macro, report with call-site function name / rule name.

IR generators also check if macro `XMALLOC_TRACE` is defined and decide if generated IR should route to traced wrappers.
