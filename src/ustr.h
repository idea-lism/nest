#pragma once

#include <stddef.h>
#include <stdint.h>

char* ustr_new(size_t sz, const char* data);
void ustr_del(char* s);

typedef enum {
  USTR_ERR_NONE,
  USTR_ERR_INVALID,
  USTR_ERR_TRUNCATED,
} UstrErr;

UstrErr ustr_find_error(size_t sz, const char* data, size_t* pos);
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
} UstrIter;

void ustr_iter_init(UstrIter* it, const char* s, int32_t char_offset);
int32_t ustr_iter_next(UstrIter* it);

int32_t ustr_decode_cp(const uint8_t* p, int32_t* adv);
