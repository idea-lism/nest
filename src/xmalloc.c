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

// Separate-chaining hash table.
// heads[]: index into nodes[] for each bucket's chain head (-1 = empty)
// nodes[]: entry storage with next-pointer chain
typedef struct {
  Pointer p;
  int32_t next; // -1 = end of chain
} Node;

typedef struct {
  int32_t* heads;
  Node* nodes;
  size_t n_buckets;
  size_t n_nodes;
  size_t node_cap;
} PointerTracker;

static PointerTracker _tracker = {0};

static size_t _hash_ptr(void* ptr, size_t n_buckets) { return ((uintptr_t)ptr >> 4) % n_buckets; }

static void _tracker_init(void) {
  _tracker.n_buckets = 1024;
  _tracker.node_cap = 1024;
  _tracker.n_nodes = 0;
  _tracker.heads = (int32_t*)malloc(_tracker.n_buckets * sizeof(int32_t));
  _tracker.nodes = (Node*)malloc(_tracker.node_cap * sizeof(Node));
  for (size_t i = 0; i < _tracker.n_buckets; i++) {
    _tracker.heads[i] = -1;
  }
}

static void _tracker_expand(void) {
  size_t old_n_buckets = _tracker.n_buckets;
  size_t old_n_nodes = _tracker.n_nodes;
  Node* old_nodes = _tracker.nodes;
  int32_t* old_heads = _tracker.heads;

  _tracker.n_buckets = old_n_buckets * 2;
  _tracker.node_cap = old_n_nodes + 256;
  _tracker.n_nodes = 0;
  _tracker.heads = (int32_t*)malloc(_tracker.n_buckets * sizeof(int32_t));
  _tracker.nodes = (Node*)malloc(_tracker.node_cap * sizeof(Node));
  for (size_t i = 0; i < _tracker.n_buckets; i++) {
    _tracker.heads[i] = -1;
  }

  // Re-insert all live entries
  for (size_t b = 0; b < old_n_buckets; b++) {
    int32_t idx = old_heads[b];
    while (idx >= 0) {
      Node* n = &old_nodes[idx];
      // add to new table
      size_t h = _hash_ptr(n->p.ptr, _tracker.n_buckets);
      size_t slot = _tracker.n_nodes++;
      _tracker.nodes[slot].p = n->p;
      _tracker.nodes[slot].next = _tracker.heads[h];
      _tracker.heads[h] = (int32_t)slot;
      idx = n->next;
    }
  }
  free(old_heads);
  free(old_nodes);
}

static void _tracker_add(void* ptr, const char* caller, int line) {
  if (!ptr) {
    return;
  }
  if (!_tracker.heads) {
    _tracker_init();
  }
  if (_tracker.n_nodes >= _tracker.node_cap) {
    _tracker_expand();
  }
  size_t h = _hash_ptr(ptr, _tracker.n_buckets);
  size_t slot = _tracker.n_nodes++;
  _tracker.nodes[slot].p.ptr = ptr;
  _tracker.nodes[slot].p.caller = caller;
  _tracker.nodes[slot].p.line = line;
  _tracker.nodes[slot].next = _tracker.heads[h];
  _tracker.heads[h] = (int32_t)slot;
}

static void _tracker_remove(void* ptr) {
  if (!ptr || !_tracker.heads) {
    return;
  }
  size_t h = _hash_ptr(ptr, _tracker.n_buckets);
  int32_t* prev_next = &_tracker.heads[h];
  int32_t cur = *prev_next;
  while (cur >= 0) {
    if (_tracker.nodes[cur].p.ptr == ptr) {
      *prev_next = _tracker.nodes[cur].next;
      // mark node as dead (optional, for leak scan)
      _tracker.nodes[cur].p.ptr = NULL;
      _tracker.nodes[cur].next = -1;
      return;
    }
    prev_next = &_tracker.nodes[cur].next;
    cur = *prev_next;
  }
}

static void _tracker_update(void* old_ptr, void* new_ptr, const char* caller, int line) {
  if (old_ptr) {
    _tracker_remove(old_ptr);
  }
  _tracker_add(new_ptr, caller, line);
}

static void _leak_check(void) {
  if (!_tracker.heads) {
    return;
  }
  int leak_count = 0;
  for (size_t b = 0; b < _tracker.n_buckets; b++) {
    int32_t idx = _tracker.heads[b];
    while (idx >= 0) {
      if (leak_count == 0) {
        fprintf(stderr, "=== XMALLOC LEAK REPORT ===\n");
      }
      fprintf(stderr, "  leaked %p from %s:%d\n", _tracker.nodes[idx].p.ptr, _tracker.nodes[idx].p.caller,
              _tracker.nodes[idx].p.line);
      leak_count++;
      idx = _tracker.nodes[idx].next;
    }
  }
  if (leak_count > 0) {
    fprintf(stderr, "=== %d leak(s) detected ===\n", leak_count);
  }
  free(_tracker.heads);
  free(_tracker.nodes);
  _tracker.heads = NULL;
  _tracker.nodes = NULL;
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
