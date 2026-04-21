#pragma once

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

// Abort-on-OOM wrappers. On failure, print caller function name:line and abort.
static inline void* xcalloc(size_t count, size_t size, const char* caller, int line) {
  void* p = calloc(count, size);
  if (!p && count && size) {
    fprintf(stderr, "xcalloc: OOM in %s:%d\n", caller, line);
    abort();
  }
  return p;
}

static inline void* xmalloc(size_t size, const char* caller, int line) {
  void* p = malloc(size);
  if (!p && size) {
    fprintf(stderr, "xmalloc: OOM in %s:%d\n", caller, line);
    abort();
  }
  return p;
}

static inline void* xrealloc(void* ptr, size_t size, const char* caller, int line) {
  void* p = realloc(ptr, size);
  if (!p && size) {
    fprintf(stderr, "xrealloc: OOM in %s:%d\n", caller, line);
    abort();
  }
  return p;
}

static inline void xfree(void* ptr) { free(ptr); }

#ifdef XMALLOC_TRACE

void* xcalloc_traced(size_t count, size_t size, const char* caller, int line);
void* xmalloc_traced(size_t size, const char* caller, int line);
void* xrealloc_traced(void* ptr, size_t size, const char* caller, int line);
void xfree_traced(void* ptr, const char* caller, int line);

#define XCALLOC(count, size) xcalloc_traced((count), (size), __FUNCTION__, __LINE__)
#define XMALLOC(size) xmalloc_traced((size), __FUNCTION__, __LINE__)
#define XREALLOC(ptr, size) xrealloc_traced((ptr), (size), __FUNCTION__, __LINE__)
#define XFREE(ptr) xfree_traced((ptr), __FUNCTION__, __LINE__)

#else

#define XCALLOC(count, size) xcalloc((count), (size), __FUNCTION__, __LINE__)
#define XMALLOC(size) xmalloc((size), __FUNCTION__, __LINE__)
#define XREALLOC(ptr, size) xrealloc((ptr), (size), __FUNCTION__, __LINE__)
#define XFREE(ptr) xfree((ptr))

#endif
