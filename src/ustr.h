#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

char* ustr_new(size_t sz, const char* data);
char* ustr_from_file(FILE* file);
void ustr_del(char* s);

typedef enum {
  USTR_ERR_NONE,
  USTR_ERR_INVALID,
  USTR_ERR_TRUNCATED,
} UstrErr;

UstrErr ustr_find_error(size_t sz, const char* data, size_t* pos);
char* ustr_slice(const char* s, int32_t cp_start, int32_t cp_end);

int32_t ustr_bytesize(const char* s);
int32_t ustr_size(const char* s);

// iterator works on the codepoint level, not byte level
typedef struct {
  const char* s;
  const uint8_t* marks;
  int32_t size;
  int32_t byte_off;
  int32_t cp_idx;
} UstrIter;

void ustr_iter_init(UstrIter* it, const char* s, int32_t cp_offset);
void ustr_iter_seek(UstrIter* it, int32_t cp_offset);
int32_t ustr_iter_next(UstrIter* it);

typedef struct {
  char buf[4];
} UstrCpBuf;
UstrCpBuf ustr_slice_cp(const char* s, int32_t cp_offset);
int32_t ustr_cp_at(const char* s, int32_t cp_offset);
int32_t ustr_encode_utf8(char* out, int32_t cp);

typedef struct {
  const char* s;
  int32_t size;
} UstrByteSlice;
UstrByteSlice ustr_slice_bytes(const char* s, int32_t cp_start, int32_t cp_end);
