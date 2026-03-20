#include "ustr.h"
#include "ustr_intern.h"
#include <stdlib.h>
#include <string.h>

// Heap layout:
//   [int32_t size][char data[size]]['\0'][uint8_t marks[(size+7)/8]]
//   ^             ^
//   heap          user pointer

static inline int32_t* ustr__size_ptr(char* s) { return (int32_t*)(s - sizeof(int32_t)); }

static inline const int32_t* ustr__size_ptr_const(const char* s) { return (const int32_t*)(s - sizeof(int32_t)); }

static inline uint8_t* ustr__marks_ptr(char* s, int32_t size) { return (uint8_t*)(s + size + 1); }

static inline const uint8_t* ustr__marks_ptr_const(const char* s, int32_t size) {
  return (const uint8_t*)(s + size + 1);
}

static inline size_t ustr__alloc_size(int32_t size) {
  return sizeof(int32_t) + (size_t)size + 1 + ((size_t)size + 7) / 8;
}

// Scalar UTF-8 DFA validator (Bjoern Hoehrmann style)

enum { S_ACC = 0, S_1, S_2, S_3, S_E0, S_ED, S_F0, S_F4, S_ERR = 8 };

// Byte class table:
//   0=ASCII, 1=cont80-8F, 2=cont90-9F, 3=contA0-BF,
//   4=C0-C1(invalid), 5=C2-DF, 6=E0, 7=E1-EC, 8=ED,
//   9=EE-EF, 10=F0, 11=F1-F3, 12=F4, 13=F5-FF(invalid)
static const uint8_t utf8_class[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 9, 9, 10, 11, 11, 11, 12, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
};

static const uint8_t utf8_trans[9][14] = {
    {S_ACC, S_ERR, S_ERR, S_ERR, S_ERR, S_1, S_E0, S_2, S_ED, S_2, S_F0, S_3, S_F4, S_ERR},
    {S_ERR, S_ACC, S_ACC, S_ACC, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR},
    {S_ERR, S_1, S_1, S_1, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR},
    {S_ERR, S_2, S_2, S_2, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR},
    {S_ERR, S_ERR, S_ERR, S_1, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR},
    {S_ERR, S_1, S_1, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR},
    {S_ERR, S_ERR, S_2, S_2, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR},
    {S_ERR, S_2, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR},
    {S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR},
};

int ustr_validate_scalar(const uint8_t* data, size_t sz, uint8_t* marks) {
  uint8_t state = S_ACC;
  for (size_t i = 0; i < sz; i++) {
    uint8_t c = utf8_class[data[i]];
    state = utf8_trans[state][c];
    if (state == S_ERR) {
      return -1;
    }
    // ASCII (class 0) or lead byte (class >= 4) starts a codepoint
    if (c == 0 || c >= 4) {
      marks[i / 8] |= (uint8_t)(1u << (i % 8));
    }
  }
  return (state == S_ACC) ? 0 : -1;
}

#ifndef __aarch64__
int ustr_validate(const uint8_t* data, size_t sz, uint8_t* marks) { return ustr_validate_scalar(data, sz, marks); }
#endif

// --- Public API ---

char* ustr_new(size_t sz, const char* data) {
  if (sz > (size_t)INT32_MAX) {
    return NULL;
  }
  int32_t size = (int32_t)sz;
  size_t alloc = ustr__alloc_size(size);
  char* heap = (char*)calloc(1, alloc);
  if (!heap) {
    return NULL;
  }

  char* s = heap + sizeof(int32_t);
  *ustr__size_ptr(s) = size;
  memcpy(s, data, sz);
  s[size] = '\0';

  uint8_t* marks = ustr__marks_ptr(s, size);
  if (ustr_validate((const uint8_t*)data, sz, marks) != 0) {
    free(heap);
    return NULL;
  }
  return s;
}

void ustr_del(char* s) {
  if (s) {
    free(s - sizeof(int32_t));
  }
}

int32_t ustr_bytesize(const char* s) { return *ustr__size_ptr_const(s); }

// --- Popcount helpers for marks traversal ---

static inline uint64_t marks_read64(const uint8_t* marks, size_t byte_off) {
  uint64_t v;
  memcpy(&v, marks + byte_off, 8);
  return v;
}

// Count set bits in marks[0 .. bit_count)
static int32_t marks_popcount(const uint8_t* marks, int32_t bit_count) {
  int32_t count = 0;
  int32_t bits = bit_count;
  size_t off = 0;

  while (bits >= 64) {
    count += __builtin_popcountll(marks_read64(marks, off));
    off += 8;
    bits -= 64;
  }
  if (bits > 0) {
    uint64_t word = 0;
    size_t remaining_bytes = ((size_t)bits + 7) / 8;
    memcpy(&word, marks + off, remaining_bytes);
    word &= ((uint64_t)1 << bits) - 1;
    count += __builtin_popcountll(word);
  }
  return count;
}

// Find byte offset of the nth codepoint start (0-indexed).
// Returns -1 if n >= total codepoints.
static int32_t marks_nth_cp(const uint8_t* marks, int32_t size, int32_t n) {
  int32_t remaining = n;
  int32_t byte_pos = 0;

  while (byte_pos + 64 <= size) {
    uint64_t word = marks_read64(marks, (size_t)byte_pos / 8);
    int pop = __builtin_popcountll(word);
    if (remaining < pop) {
      break;
    }
    remaining -= pop;
    byte_pos += 64;
  }

  size_t mark_off = (size_t)byte_pos / 8;
  int32_t bits_left = size - byte_pos;
  uint64_t word = 0;
  size_t read_bytes = bits_left >= 64 ? 8 : ((size_t)bits_left + 7) / 8;
  if (read_bytes > 0) {
    memcpy(&word, marks + mark_off, read_bytes);
  }
  if (bits_left < 64 && bits_left > 0) {
    word &= ((uint64_t)1 << bits_left) - 1;
  }

  while (word) {
    if (remaining == 0) {
      return byte_pos + __builtin_ctzll(word);
    }
    word &= word - 1;
    remaining--;
  }
  return -1;
}

int32_t ustr_size(const char* s) {
  int32_t size = ustr_bytesize(s);
  const uint8_t* marks = ustr__marks_ptr_const(s, size);
  return marks_popcount(marks, size);
}

// --- Iterator ---

void ustr_iter_init(ustr_iter* it, const char* s, int32_t byte_offset) {
  int32_t size = ustr_bytesize(s);
  it->s = s;
  it->size = size;
  it->marks = ustr__marks_ptr_const(s, size);
  it->byte_off = byte_offset;
  it->line = 0;
  it->col = 0;
  it->cp_idx = marks_popcount(it->marks, byte_offset);
}

static inline int32_t decode_cp(const uint8_t* p, int32_t* adv) {
  uint8_t b = p[0];
  if (b < 0x80) {
    *adv = 1;
    return b;
  } else if (b < 0xE0) {
    *adv = 2;
    return ((int32_t)(b & 0x1F) << 6) | (p[1] & 0x3F);
  } else if (b < 0xF0) {
    *adv = 3;
    return ((int32_t)(b & 0x0F) << 12) | ((int32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
  } else {
    *adv = 4;
    return ((int32_t)(b & 0x07) << 18) | ((int32_t)(p[1] & 0x3F) << 12) | ((int32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
  }
}

int32_t ustr_iter_next(ustr_iter* it) {
  if (it->byte_off >= it->size) {
    return -1;
  }

  int32_t adv;
  int32_t cp = decode_cp((const uint8_t*)it->s + it->byte_off, &adv);
  it->byte_off += adv;
  it->cp_idx++;

  if (cp == '\n') {
    it->line++;
    it->col = 0;
  } else {
    it->col++;
  }
  return cp;
}

// --- Slicing ---

char* ustr_slice(const char* s, int32_t start, int32_t end) {
  int32_t size = ustr_bytesize(s);
  int32_t cplen = ustr_size(s);
  const uint8_t* marks = ustr__marks_ptr_const(s, size);

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

  int32_t byte_start = marks_nth_cp(marks, size, start);
  int32_t byte_end = (end >= cplen) ? size : marks_nth_cp(marks, size, end);
  if (byte_start < 0) {
    return ustr_new(0, "");
  }
  if (byte_end < 0) {
    byte_end = size;
  }

  return ustr_new((size_t)(byte_end - byte_start), s + byte_start);
}
