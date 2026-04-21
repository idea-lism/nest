#include "ustr.h"
#include "ustr_intern.h"
#include "xmalloc.h"
#include <assert.h>
#include <string.h>

static int32_t* _size_ptr(char* s) { return (int32_t*)(s - sizeof(int32_t)); }

static const int32_t* _size_ptr_const(const char* s) { return (const int32_t*)(s - sizeof(int32_t)); }

static uint8_t* _marks_ptr(char* s, int32_t size) { return (uint8_t*)(s + size + 1); }

static const uint8_t* _marks_ptr_const(const char* s, int32_t size) { return (const uint8_t*)(s + size + 1); }

static size_t _alloc_size(int32_t size) { return sizeof(int32_t) + (size_t)size + 1 + ((size_t)size + 7) / 8; }

enum { S_ACC = 0, S_1, S_2, S_3, S_E0, S_ED, S_F0, S_F4, S_ERR = 8 };

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
    if (c == 0 || c >= 4) {
      marks[i / 8] |= (uint8_t)(1u << (i % 8));
    }
  }
  return (state == S_ACC) ? 0 : -1;
}

#if !defined(__aarch64__) && !defined(__AVX2__)
int ustr_validate(const uint8_t* data, size_t sz, uint8_t* marks) { return ustr_validate_scalar(data, sz, marks); }
#endif

UstrErr ustr_find_error(size_t sz, const char* data, size_t* pos) {
  const uint8_t* d = (const uint8_t*)data;
  uint8_t state = S_ACC;
  size_t seq_start = 0;
  for (size_t i = 0; i < sz; i++) {
    if (state == S_ACC) {
      seq_start = i;
    }
    state = utf8_trans[state][utf8_class[d[i]]];
    if (state == S_ERR) {
      *pos = seq_start;
      return USTR_ERR_INVALID;
    }
  }
  if (state != S_ACC) {
    *pos = seq_start;
    return USTR_ERR_TRUNCATED;
  }
  return USTR_ERR_NONE;
}

char* ustr_new(size_t sz, const char* data) {
  if (sz > (size_t)INT32_MAX) {
    return NULL;
  }
  int32_t size = (int32_t)sz;
  size_t alloc = _alloc_size(size);
  char* heap = (char*)XCALLOC(1, alloc);
  if (!heap) {
    return NULL;
  }

  char* s = heap + sizeof(int32_t);
  *_size_ptr(s) = size;
  memcpy(s, data, sz);
  s[size] = '\0';

  uint8_t* marks = _marks_ptr(s, size);
  if (ustr_validate((const uint8_t*)data, sz, marks) != 0) {
    XFREE(heap);
    return NULL;
  }
  return s;
}

char* ustr_from_file(FILE* file) {
  if (fseek(file, 0, SEEK_END) != 0) {
    fprintf(stderr, "ustr_from_file: fseek SEEK_END failed\n");
    return NULL;
  }
  long len = ftell(file);
  if (len < 0) {
    fprintf(stderr, "ustr_from_file: ftell failed\n");
    return NULL;
  }
  if ((unsigned long)len > (size_t)INT32_MAX) {
    fprintf(stderr, "ustr_from_file: file too large (%ld bytes)\n", len);
    return NULL;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fprintf(stderr, "ustr_from_file: fseek SEEK_SET failed\n");
    return NULL;
  }
  int32_t size = (int32_t)len;
  char* heap = (char*)XCALLOC(1, _alloc_size(size));
  if (!heap) {
    fprintf(stderr, "ustr_from_file: alloc failed (%d bytes)\n", size);
    return NULL;
  }
  char* s = heap + sizeof(int32_t);
  *_size_ptr(s) = size;
  if (size > 0 && (int32_t)fread(s, 1, (size_t)size, file) != size) {
    fprintf(stderr, "ustr_from_file: fread failed\n");
    XFREE(heap);
    return NULL;
  }
  s[size] = '\0';
  uint8_t* marks = _marks_ptr(s, size);
  if (ustr_validate((const uint8_t*)s, (size_t)size, marks) != 0) {
    size_t err_pos;
    UstrErr err = ustr_find_error((size_t)size, s, &err_pos);
    const char* reason = (err == USTR_ERR_TRUNCATED) ? "truncated" : "invalid";
    fprintf(stderr, "ustr_from_file: %s UTF-8 at byte %zu\n", reason, err_pos);
    XFREE(heap);
    return NULL;
  }
  return s;
}

void ustr_del(char* s) {
  if (s) {
    XFREE(s - sizeof(int32_t));
  }
}

int32_t ustr_bytesize(const char* s) { return *_size_ptr_const(s); }

static uint64_t _marks_read64(const uint8_t* marks, size_t byte_off) {
  uint64_t v;
  memcpy(&v, marks + byte_off, 8);
  return v;
}

static int32_t _marks_popcount(const uint8_t* marks, int32_t bit_count) {
  int32_t count = 0;
  int32_t bits = bit_count;
  size_t off = 0;

  while (bits >= 64) {
    count += __builtin_popcountll(_marks_read64(marks, off));
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

static int32_t _marks_nth_cp(const uint8_t* marks, int32_t size, int32_t n) {
  int32_t remaining = n;
  int32_t byte_pos = 0;

  while (byte_pos + 64 <= size) {
    uint64_t word = _marks_read64(marks, (size_t)byte_pos / 8);
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
  const uint8_t* marks = _marks_ptr_const(s, size);
  return _marks_popcount(marks, size);
}

void ustr_iter_init(UstrIter* it, const char* s, int32_t char_offset) {
  int32_t bytesize = ustr_bytesize(s);
  const uint8_t* marks = _marks_ptr_const(s, bytesize);
  int32_t byte_off;
  if (char_offset == 0) {
    byte_off = 0;
  } else {
    byte_off = _marks_nth_cp(marks, bytesize, char_offset);
    if (byte_off < 0) {
      int32_t cplen = _marks_popcount(marks, bytesize);
      assert(char_offset == cplen);
      byte_off = bytesize;
    }
  }
  it->s = s;
  it->bytesize = bytesize;
  it->marks = marks;
  it->byte_index = byte_off;
  it->cp_index = char_offset;
}

void ustr_iter_seek(UstrIter* it, int32_t cp_offset) {
  if (cp_offset == it->cp_index) {
    return;
  }
  if (cp_offset + 64 > it->cp_index && cp_offset < it->cp_index) {
    // close backtrack: scan marks backwards from current byte_off
    int32_t delta = it->cp_index - cp_offset;
    int32_t byte_pos = it->byte_index;
    while (delta > 0 && byte_pos > 0) {
      byte_pos--;
      int32_t mark_byte = byte_pos / 8;
      int32_t mark_bit = byte_pos % 8;
      if (it->marks[mark_byte] & (1u << mark_bit)) {
        delta--;
      }
    }
    it->byte_index = byte_pos;
    it->cp_index = cp_offset;
  } else {
    // far seek or forward: re-init from 0
    ustr_iter_init(it, it->s, cp_offset);
  }
}

static int32_t _decode_cp(const char* p, int32_t* out_decoded_bytes) {
  uint8_t b = (uint8_t)p[0];
  if (b < 0x80) {
    *out_decoded_bytes = 1;
    return b;
  } else if (b < 0xE0) {
    *out_decoded_bytes = 2;
    return ((int32_t)(b & 0x1F) << 6) | (p[1] & 0x3F);
  } else if (b < 0xF0) {
    *out_decoded_bytes = 3;
    return ((int32_t)(b & 0x0F) << 12) | ((int32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
  } else {
    *out_decoded_bytes = 4;
    return ((int32_t)(b & 0x07) << 18) | ((int32_t)(p[1] & 0x3F) << 12) | ((int32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
  }
}

int32_t ustr_iter_next(UstrIter* it) {
  if (it->byte_index >= it->bytesize) {
    return -1;
  }

  int32_t adv;
  int32_t cp = _decode_cp(it->s + it->byte_index, &adv);
  it->byte_index += adv;
  it->cp_index++;

  return cp;
}

int32_t ustr_cp_at(const char* s, int32_t cp_offset) {
  int32_t size = ustr_bytesize(s);
  const uint8_t* marks = _marks_ptr_const(s, size);
  int32_t byte_off;
  if (cp_offset == 0) {
    byte_off = 0;
  } else {
    byte_off = _marks_nth_cp(marks, size, cp_offset);
    if (byte_off < 0) {
      int32_t cplen = _marks_popcount(marks, size);
      assert(cp_offset == cplen);
      return -1;
    }
  }
  int32_t adv;
  return _decode_cp(s + byte_off, &adv);
}

UstrCpBuf ustr_slice_cp(const char* s, int32_t char_offset) {
  UstrCpBuf r = {.buf = {0}};

  int32_t size = ustr_bytesize(s);
  const uint8_t* marks = _marks_ptr_const(s, size);
  int32_t byte_off;
  if (char_offset == 0) {
    byte_off = 0;
  } else {
    byte_off = _marks_nth_cp(marks, size, char_offset);
    if (byte_off < 0) {
      int32_t cplen = _marks_popcount(marks, size);
      assert(char_offset == cplen);
      byte_off = size;
    }
  }

  const uint8_t* p = (const uint8_t*)s + byte_off;
  uint8_t b = (uint8_t)p[0];
  int buf_sz = 0;
  if (b < 0x80) {
    r.buf[buf_sz++] = (char)b;
  } else if (b < 0xE0) {
    r.buf[buf_sz++] = (char)b;
    r.buf[buf_sz++] = (char)p[1];
  } else if (b < 0xF0) {
    r.buf[buf_sz++] = (char)b;
    r.buf[buf_sz++] = (char)p[1];
    r.buf[buf_sz++] = (char)p[2];
  } else {
    r.buf[buf_sz++] = (char)b;
    r.buf[buf_sz++] = (char)p[1];
    r.buf[buf_sz++] = (char)p[2];
    r.buf[buf_sz++] = (char)p[3];
  }

  r.buf[buf_sz] = '\0';
  return r;
}

int32_t ustr_encode_utf8(char* out, int32_t cp) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  } else if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  } else if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  } else {
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
  }
}

char* ustr_slice(const char* s, int32_t start, int32_t end) {
  int32_t size = ustr_bytesize(s);
  int32_t cplen = ustr_size(s);
  const uint8_t* marks = _marks_ptr_const(s, size);

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

  int32_t byte_start = _marks_nth_cp(marks, size, start);
  int32_t byte_end = (end >= cplen) ? size : _marks_nth_cp(marks, size, end);
  if (byte_start < 0) {
    return ustr_new(0, "");
  }
  if (byte_end < 0) {
    byte_end = size;
  }

  return ustr_new((size_t)(byte_end - byte_start), s + byte_start);
}

UstrByteSlice ustr_slice_bytes(const char* s, int32_t cp_start, int32_t cp_end) {
  int32_t size = ustr_bytesize(s);
  int32_t cplen = ustr_size(s);
  const uint8_t* marks = _marks_ptr_const(s, size);

  if (cp_start < 0) {
    cp_start += cplen;
  }
  if (cp_end < 0) {
    cp_end += cplen;
  }
  if (cp_start < 0) {
    cp_start = 0;
  }
  if (cp_end > cplen) {
    cp_end = cplen;
  }
  if (cp_start >= cp_end) {
    return (UstrByteSlice){.s = s, .size = size};
  }

  int32_t byte_start = _marks_nth_cp(marks, size, cp_start);
  int32_t byte_end = (cp_end >= cplen) ? size : _marks_nth_cp(marks, size, cp_end);
  if (byte_start < 0) {
    byte_start = size;
  }
  if (byte_end < 0) {
    byte_end = size;
  }

  return (UstrByteSlice){.s = s + byte_start, .size = byte_end - byte_start};
}
