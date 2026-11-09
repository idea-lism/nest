# XMALLOC

`xmalloc.h` defines allocation macros that abort on OOM with a call-site message: `__FUNCTION__:__LINE__`.

```c
// --- Untraced (default) ---

void* xcalloc(size_t count, size_t size, const char* caller, int line);
void* xmalloc(size_t size, const char* caller, int line);
void* xrealloc(void* ptr, size_t size, const char* caller, int line);
void  xfree(void* ptr);

// Call these; the caller/line arguments are filled in by the macro.
#define XCALLOC(count, size) /* xcalloc(count, size, __FUNCTION__, __LINE__) */
#define XMALLOC(size)        /* xmalloc(size, __FUNCTION__, __LINE__) */
#define XREALLOC(ptr, size)  /* xrealloc(ptr, size, __FUNCTION__, __LINE__) */
#define XFREE(ptr)           /* xfree(ptr) */

// --- Traced (XMALLOC_TRACE) ---

void* xcalloc_traced(size_t count, size_t size, const char* caller, int line);
void* xmalloc_traced(size_t size, const char* caller, int line);
void* xrealloc_traced(void* ptr, size_t size, const char* caller, int line);
void  xfree_traced(void* ptr, const char* caller, int line);

// The macros route to the *_traced variants and keep the call site.
#define XCALLOC(count, size) /* xcalloc_traced(...) */
#define XMALLOC(size)        /* xmalloc_traced(...) */
#define XREALLOC(ptr, size)  /* xrealloc_traced(...) */
#define XFREE(ptr)           /* xfree_traced(...) */
```

With `XMALLOC_TRACE`, `xmalloc.c` installs an `atexit()` hook via `__attribute__((constructor))` and reports leaks with the original call site.

Leak tracking data structure (hash table on allocated pointer; simple hash, linked-list collision resolution):

```c
typedef struct {
  void* ptr;
  // call site info
  const char* caller;
  int line;
} Pointer;

typedef struct {
  Pointer p;           // ptr == 0 when empty
  PointerBucket* next; // NULL when no next
} PointerBucket;

typedef struct {
  size_t bucket_cap;               // expand at 65% fill rate
  PointerBucket* pointer_buckets;  // the buffer
} PointerTracker;
```
