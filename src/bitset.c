#include "bitset.h"
#include "xmalloc.h"

#include <string.h>

struct Bitset {
  uint64_t* chunks;
  uint32_t n_chunks;
};

static uint32_t _chunk_index(uint32_t offset) { return offset / 64; }
static uint64_t _chunk_mask(uint32_t offset) { return (uint64_t)1 << (offset % 64); }

static void _ensure_capacity(Bitset* bs, uint32_t chunk_idx) {
  if (chunk_idx < bs->n_chunks) {
    return;
  }
  uint32_t new_n = chunk_idx + 1;
  bs->chunks = XREALLOC(bs->chunks, new_n * sizeof(uint64_t));
  memset(bs->chunks + bs->n_chunks, 0, (new_n - bs->n_chunks) * sizeof(uint64_t));
  bs->n_chunks = new_n;
}

Bitset* bitset_new(void) { return XCALLOC(1, sizeof(Bitset)); }

void bitset_del(Bitset* bs) {
  if (!bs) {
    return;
  }
  XFREE(bs->chunks);
  XFREE(bs);
}

void bitset_add_bit(Bitset* bs, uint32_t offset) {
  uint32_t ci = _chunk_index(offset);
  _ensure_capacity(bs, ci);
  bs->chunks[ci] |= _chunk_mask(offset);
}

void bitset_clear_bit(Bitset* bs, uint32_t offset) {
  uint32_t ci = _chunk_index(offset);
  if (ci >= bs->n_chunks) {
    return;
  }
  bs->chunks[ci] &= ~_chunk_mask(offset);
}

bool bitset_contains(Bitset* bs, uint32_t offset) {
  uint32_t ci = _chunk_index(offset);
  if (ci >= bs->n_chunks) {
    return false;
  }
  return (bs->chunks[ci] & _chunk_mask(offset)) != 0;
}

Bitset* bitset_or(Bitset* s1, Bitset* s2) {
  uint32_t max_n = s1->n_chunks > s2->n_chunks ? s1->n_chunks : s2->n_chunks;
  Bitset* result = bitset_new();
  if (max_n == 0) {
    return result;
  }
  result->chunks = XCALLOC(max_n, sizeof(uint64_t));
  result->n_chunks = max_n;
  for (uint32_t i = 0; i < max_n; i++) {
    uint64_t a = i < s1->n_chunks ? s1->chunks[i] : 0;
    uint64_t b = i < s2->n_chunks ? s2->chunks[i] : 0;
    result->chunks[i] = a | b;
  }
  return result;
}

void bitset_or_into(Bitset* dest, Bitset* src) {
  for (uint32_t i = 0; i < src->n_chunks; i++) {
    if (src->chunks[i]) {
      _ensure_capacity(dest, i);
      dest->chunks[i] |= src->chunks[i];
    }
  }
}

Bitset* bitset_and(Bitset* s1, Bitset* s2) {
  uint32_t min_n = s1->n_chunks < s2->n_chunks ? s1->n_chunks : s2->n_chunks;
  Bitset* result = bitset_new();
  if (min_n == 0) {
    return result;
  }
  result->chunks = XCALLOC(min_n, sizeof(uint64_t));
  result->n_chunks = min_n;
  for (uint32_t i = 0; i < min_n; i++) {
    result->chunks[i] = s1->chunks[i] & s2->chunks[i];
  }
  return result;
}

bool bitset_equal(Bitset* a, Bitset* b) {
  uint32_t max_n = a->n_chunks > b->n_chunks ? a->n_chunks : b->n_chunks;
  for (uint32_t i = 0; i < max_n; i++) {
    uint64_t ca = i < a->n_chunks ? a->chunks[i] : 0;
    uint64_t cb = i < b->n_chunks ? b->chunks[i] : 0;
    if (ca != cb) {
      return false;
    }
  }
  return true;
}

uint32_t bitset_size(Bitset* bs) {
  uint32_t count = 0;
  for (uint32_t i = 0; i < bs->n_chunks; i++) {
    count += (uint32_t)__builtin_popcountll(bs->chunks[i]);
  }
  return count;
}
