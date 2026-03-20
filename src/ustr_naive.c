// Naive (bit-by-bit) implementations of size/slice for benchmarking.
// Not part of the public library.

#include "ustr.h"
#include "ustr_intern.h"
#include <string.h>

// These duplicate the internal accessors from ustr.c -- kept minimal.
static inline int32_t naive__bytesize(const char* s) { return *(const int32_t*)(s - sizeof(int32_t)); }

static inline const uint8_t* naive__marks(const char* s, int32_t size) { return (const uint8_t*)(s + size + 1); }

int32_t ustr_size_naive(const char* s) {
  int32_t size = naive__bytesize(s);
  const uint8_t* marks = naive__marks(s, size);
  int32_t count = 0;
  for (int32_t i = 0; i < size; i++) {
    if (marks[i / 8] & (1u << (i % 8))) {
      count++;
    }
  }
  return count;
}

char* ustr_slice_naive(const char* s, int32_t start, int32_t end) {
  int32_t size = naive__bytesize(s);
  int32_t cplen = ustr_size_naive(s);
  const uint8_t* marks = naive__marks(s, size);

  if (start < 0) {
    start += cplen;
  }
  if (end < 0) {
    end += cplen;
  }
  if (start < 0) {
    start = 0;
  }
  if (end > cplen) {
    end = cplen;
  }
  if (start >= end) {
    return ustr_new(0, "");
  }

  int32_t cp = 0;
  int32_t byte_start = -1, byte_end = -1;
  for (int32_t i = 0; i <= size && cp <= end; i++) {
    int is_mark = (i < size) && (marks[i / 8] & (1u << (i % 8)));
    if (is_mark) {
      if (cp == start) {
        byte_start = i;
      }
      if (cp == end) {
        byte_end = i;
      }
      cp++;
    }
  }
  if (byte_start < 0) {
    return ustr_new(0, "");
  }
  if (byte_end < 0) {
    byte_end = size;
  }

  return ustr_new((size_t)(byte_end - byte_start), s + byte_start);
}
