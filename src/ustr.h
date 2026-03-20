#pragma once

#include <stddef.h>
#include <stdint.h>

// Fat-pointer UTF-8 string.
//
// Heap layout:
//   [int32_t size][char data[size]]['\0'][char marks[(size+7)/8]]
//
// The user-visible pointer points to &data[0].
// marks bitset: bit i of marks[i/8] (LSB-first) = 1 if byte i starts a codepoint.

char* ustr_new(size_t sz, const char* data);
void ustr_del(char* s);
char* ustr_slice(const char* s, int32_t start, int32_t end);

int32_t ustr_bytesize(const char* s);
int32_t ustr_size(const char* s);

typedef struct {
  const char* s;
  int32_t size;
  const uint8_t* marks;
  int32_t byte_off;
  int32_t cp_idx;
  int32_t line;
  int32_t col;
} ustr_iter;

void ustr_iter_init(ustr_iter* it, const char* s, int32_t byte_offset);
int32_t ustr_iter_next(ustr_iter* it);
