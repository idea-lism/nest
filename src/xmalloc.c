#include "xmalloc.h"

#ifdef XMALLOC_TRACE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  void* ptr;
  const char* caller;
  int line;
} Pointer;

typedef struct PointerBucket {
  Pointer p;              // ptr set to 0 when empty
  struct PointerBucket* next; // for collision resolution, null when no next
} PointerBucket;

typedef struct {
  size_t bucket_cap;          // expand when fill rate at 65%
  size_t fill;                // number of live entries
  PointerBucket* pointer_buckets; // the buffer
} PointerTracker;

static PointerTracker _tracker = {0};

static size_t _hash_ptr(void* ptr, size_t bucket_cap) { return ((uintptr_t)ptr >> 4) % bucket_cap; }

static void _tracker_init(void) {
  _tracker.bucket_cap = 1024;
  _tracker.fill = 0;
  _tracker.pointer_buckets = (PointerBucket*)calloc(_tracker.bucket_cap, sizeof(PointerBucket));
}

static void _tracker_expand(void) {
  size_t old_cap = _tracker.bucket_cap;
  PointerBucket* old_buckets = _tracker.pointer_buckets;

  _tracker.bucket_cap = old_cap * 2;
  _tracker.fill = 0;
  _tracker.pointer_buckets = (PointerBucket*)calloc(_tracker.bucket_cap, sizeof(PointerBucket));

  // Re-insert all live entries
  for (size_t i = 0; i < old_cap; i++) {
    PointerBucket* b = &old_buckets[i];
    while (b) {
      if (b->p.ptr) {
        size_t h = _hash_ptr(b->p.ptr, _tracker.bucket_cap);
        PointerBucket* dst = &_tracker.pointer_buckets[h];
        if (!dst->p.ptr) {
          dst->p = b->p;
        } else {
          PointerBucket* node = (PointerBucket*)malloc(sizeof(PointerBucket));
          node->p = b->p;
          node->next = dst->next;
          dst->next = node;
        }
        _tracker.fill++;
      }
      b = b->next;
    }
  }

  // Free old chains
  for (size_t i = 0; i < old_cap; i++) {
    PointerBucket* chain = old_buckets[i].next;
    while (chain) {
      PointerBucket* tmp = chain;
      chain = chain->next;
      free(tmp);
    }
  }
  free(old_buckets);
}

static void _tracker_add(void* ptr, const char* caller, int line) {
  if (!ptr) {
    return;
  }
  if (!_tracker.pointer_buckets) {
    _tracker_init();
  }
  // expand at 65% fill rate
  if (_tracker.fill * 100 / _tracker.bucket_cap >= 65) {
    _tracker_expand();
  }
  size_t h = _hash_ptr(ptr, _tracker.bucket_cap);
  PointerBucket* b = &_tracker.pointer_buckets[h];
  if (!b->p.ptr) {
    b->p.ptr = ptr;
    b->p.caller = caller;
    b->p.line = line;
  } else {
    PointerBucket* node = (PointerBucket*)malloc(sizeof(PointerBucket));
    node->p.ptr = ptr;
    node->p.caller = caller;
    node->p.line = line;
    node->next = b->next;
    b->next = node;
  }
  _tracker.fill++;
}

static void _tracker_remove(void* ptr) {
  if (!ptr || !_tracker.pointer_buckets) {
    return;
  }
  size_t h = _hash_ptr(ptr, _tracker.bucket_cap);
  PointerBucket* b = &_tracker.pointer_buckets[h];
  // Check head bucket
  if (b->p.ptr == ptr) {
    if (b->next) {
      // Pull next node into head slot
      PointerBucket* next = b->next;
      b->p = next->p;
      b->next = next->next;
      free(next);
    } else {
      b->p.ptr = NULL;
    }
    _tracker.fill--;
    return;
  }
  // Walk chain
  PointerBucket* prev = b;
  PointerBucket* cur = b->next;
  while (cur) {
    if (cur->p.ptr == ptr) {
      prev->next = cur->next;
      free(cur);
      _tracker.fill--;
      return;
    }
    prev = cur;
    cur = cur->next;
  }
}

static void _tracker_update(void* old_ptr, void* new_ptr, const char* caller, int line) {
  if (old_ptr) {
    _tracker_remove(old_ptr);
  }
  _tracker_add(new_ptr, caller, line);
}

static void _leak_check(void) {
  if (!_tracker.pointer_buckets) {
    return;
  }
  int leak_count = 0;
  for (size_t i = 0; i < _tracker.bucket_cap; i++) {
    PointerBucket* b = &_tracker.pointer_buckets[i];
    while (b) {
      if (b->p.ptr) {
        if (leak_count == 0) {
          fprintf(stderr, "=== XMALLOC LEAK REPORT ===\n");
        }
        fprintf(stderr, "  leaked %p from %s:%d\n", b->p.ptr, b->p.caller, b->p.line);
        leak_count++;
      }
      b = b->next;
    }
  }
  if (leak_count > 0) {
    fprintf(stderr, "=== %d leak(s) detected ===\n", leak_count);
  }
  // Free chains
  for (size_t i = 0; i < _tracker.bucket_cap; i++) {
    PointerBucket* chain = _tracker.pointer_buckets[i].next;
    while (chain) {
      PointerBucket* tmp = chain;
      chain = chain->next;
      free(tmp);
    }
  }
  free(_tracker.pointer_buckets);
  _tracker.pointer_buckets = NULL;
}

__attribute__((constructor)) static void _xmalloc_init(void) { atexit(_leak_check); }

void* xcalloc_traced(size_t count, size_t size, const char* caller, int line) {
  void* p = xcalloc(count, size, caller, line);
  _tracker_add(p, caller, line);
  return p;
}

void* xmalloc_traced(size_t size, const char* caller, int line) {
  void* p = xmalloc(size, caller, line);
  _tracker_add(p, caller, line);
  return p;
}

void* xrealloc_traced(void* ptr, size_t size, const char* caller, int line) {
  void* p = xrealloc(ptr, size, caller, line);
  _tracker_update(ptr, p, caller, line);
  return p;
}

void xfree_traced(void* ptr, const char* caller, int line) {
  (void)caller;
  (void)line;
  _tracker_remove(ptr);
  xfree(ptr);
}

#endif
