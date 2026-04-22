# XMALLOC

`xmalloc.h` defines XMALLOC/XFREE macro that, when `calloc`/`malloc`/`realloc` fails (OOM), `abort()` with call-site message: `__FUNCTION__:__LINE__`

when defined macro `XMALLOC_TRACE` the macros routes to traced versions: `calloc_traced`/`xmalloc_traced`/`xrealloc_traced`/`xfree_traced`.

when defined macro `XMALLOC_TRACE`, `xmalloc.c` has an initializer that defines `atexit()` hook at `__attribute__((constructor))` to check if there are leaks. Leak reporting will include the call site info too.

Leak tracking data structure (a hash table on allocated ptr, choose a simple hashing function, use linked list for collision resolution):

```c
typedef struct {
  void* ptr;
  // call site info
  const char* caller;
  int line;
} Pointer;

typedef struct {
  Pointer p; // ptr set to 0 when empty
  PointerBucket* next; // for collision relsolution, null when no next
} PointerBucket;

typedef struct {
  size_t bucket_cap; // expand when fill rate at 65%
  PointerBucket* pointer_buckets; // the buffer
} PointerTracker;
```

# LLVM IR Adaption

For calloc/malloc/realloc/free in generated LLVM IR, also implement a similar macro, report with call-site function name / rule name.

IR generators also check if macro `XMALLOC_TRACE` is defined and decide if generated IR should route to traced wrappers.
