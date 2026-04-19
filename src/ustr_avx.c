#ifdef __AVX2__

#include "ustr.h"
#include "ustr_intern.h"
#include <immintrin.h>
#include <string.h>

// AVX2-accelerated UTF-8 validation with simultaneous marks bitset generation.
//
// Port of the NEON implementation (ustr_neon.c) using the same simdutf "lookup4" algorithm.
// Processes 32 bytes per iteration instead of 16.

#define TOO_SHORT (1 << 0)
#define TOO_LONG (1 << 1)
#define OVERLONG_3 (1 << 2)
#define TOO_LARGE (1 << 3)
#define SURROGATE (1 << 4)
#define OVERLONG_2 (1 << 5)
#define TOO_LARGE_1000 (1 << 6)
#define OVERLONG_4 (1 << 6)
#define TWO_CONTS (1 << 7)
#define CARRY (TOO_SHORT | TOO_LONG | TWO_CONTS)

// Tables are duplicated across both 128-bit lanes for _mm256_shuffle_epi8.

static __m256i _load_tbl(const uint8_t tbl[16]) {
  __m128i lo = _mm_loadu_si128((const __m128i*)tbl);
  return _mm256_broadcastsi128_si256(lo);
}

static const uint8_t byte_1_high_tbl[16] = {
    TOO_LONG,
    TOO_LONG,
    TOO_LONG,
    TOO_LONG,
    TOO_LONG,
    TOO_LONG,
    TOO_LONG,
    TOO_LONG,
    TWO_CONTS,
    TWO_CONTS,
    TWO_CONTS,
    TWO_CONTS,
    TOO_SHORT | OVERLONG_2,
    TOO_SHORT,
    TOO_SHORT | OVERLONG_3 | SURROGATE,
    TOO_SHORT | TOO_LARGE | TOO_LARGE_1000 | OVERLONG_4,
};

static const uint8_t byte_1_low_tbl[16] = {
    CARRY | OVERLONG_3 | OVERLONG_2 | OVERLONG_4,
    CARRY | OVERLONG_2,
    CARRY,
    CARRY,
    CARRY | TOO_LARGE,
    CARRY | TOO_LARGE | TOO_LARGE_1000,
    CARRY | TOO_LARGE | TOO_LARGE_1000,
    CARRY | TOO_LARGE | TOO_LARGE_1000,
    CARRY | TOO_LARGE | TOO_LARGE_1000,
    CARRY | TOO_LARGE | TOO_LARGE_1000,
    CARRY | TOO_LARGE | TOO_LARGE_1000,
    CARRY | TOO_LARGE | TOO_LARGE_1000,
    CARRY | TOO_LARGE | TOO_LARGE_1000,
    CARRY | TOO_LARGE | TOO_LARGE_1000 | SURROGATE,
    CARRY | TOO_LARGE | TOO_LARGE_1000,
    CARRY | TOO_LARGE | TOO_LARGE_1000,
};

static const uint8_t byte_2_high_tbl[16] = {
    TOO_SHORT,
    TOO_SHORT,
    TOO_SHORT,
    TOO_SHORT,
    TOO_SHORT,
    TOO_SHORT,
    TOO_SHORT,
    TOO_SHORT,
    TOO_LONG | OVERLONG_2 | TWO_CONTS | OVERLONG_3 | TOO_LARGE_1000 | OVERLONG_4,
    TOO_LONG | OVERLONG_2 | TWO_CONTS | OVERLONG_3 | TOO_LARGE,
    TOO_LONG | OVERLONG_2 | TWO_CONTS | SURROGATE | TOO_LARGE,
    TOO_LONG | OVERLONG_2 | TWO_CONTS | SURROGATE | TOO_LARGE,
    TOO_SHORT,
    TOO_SHORT,
    TOO_SHORT,
    TOO_SHORT,
};

// _mm256_shuffle_epi8 uses only the low 4 bits as index (and zeros lane if bit 7 is set).
// So we can use it as a 16-entry table lookup per 128-bit lane.

static __m256i _check_special_cases(__m256i input, __m256i prev1) {
  __m256i b1h_tbl = _load_tbl(byte_1_high_tbl);
  __m256i b1l_tbl = _load_tbl(byte_1_low_tbl);
  __m256i b2h_tbl = _load_tbl(byte_2_high_tbl);

  __m256i mask4 = _mm256_set1_epi8(0x0F);
  __m256i prev1_hi = _mm256_and_si256(_mm256_srli_epi16(prev1, 4), mask4);
  __m256i prev1_lo = _mm256_and_si256(prev1, mask4);
  __m256i cur_hi = _mm256_and_si256(_mm256_srli_epi16(input, 4), mask4);

  __m256i byte_1_high = _mm256_shuffle_epi8(b1h_tbl, prev1_hi);
  __m256i byte_1_low = _mm256_shuffle_epi8(b1l_tbl, prev1_lo);
  __m256i byte_2_high = _mm256_shuffle_epi8(b2h_tbl, cur_hi);

  return _mm256_and_si256(_mm256_and_si256(byte_1_high, byte_1_low), byte_2_high);
}

// Shift input right by N bytes across the 256-bit boundary, filling from prev_input.
// prev_combined = [prev_input[16..31], input[0..15]]
// Then _mm256_alignr_epi8(input, prev_combined, 16-N) gives the cross-chunk shift.
static __m256i _prev_n(__m256i prev_combined, __m256i input, int n) {
  // _mm256_alignr_epi8 operates per 128-bit lane:
  //   low lane:  alignr(input[0..15],  prev_input[16..31], 16-n)
  //   high lane: alignr(input[16..31], input[0..15],       16-n)
  switch (n) {
  case 1:
    return _mm256_alignr_epi8(input, prev_combined, 15);
  case 2:
    return _mm256_alignr_epi8(input, prev_combined, 14);
  case 3:
    return _mm256_alignr_epi8(input, prev_combined, 13);
  default:
    __builtin_unreachable();
  }
}

static __m256i _check_multibyte_lengths(__m256i input, __m256i prev_combined, __m256i sc) {
  __m256i prev2 = _prev_n(prev_combined, input, 2);
  __m256i prev3 = _prev_n(prev_combined, input, 3);

  // unsigned >= via max trick: a >= b iff max(a, b) == a
  __m256i threshold_e0 = _mm256_set1_epi8((char)0xE0);
  __m256i threshold_f0 = _mm256_set1_epi8((char)0xF0);
  __m256i is_third = _mm256_cmpeq_epi8(_mm256_max_epu8(prev2, threshold_e0), prev2);
  __m256i is_fourth = _mm256_cmpeq_epi8(_mm256_max_epu8(prev3, threshold_f0), prev3);

  __m256i must23 = _mm256_xor_si256(is_third, is_fourth);
  __m256i must23_80 = _mm256_and_si256(must23, _mm256_set1_epi8((char)0x80));
  return _mm256_xor_si256(must23_80, sc);
}

static __m256i _is_incomplete(__m256i input) {
  // Only the last 3 bytes matter; rest are 0xFF so vcgt always false.
  static const uint8_t max_arr[32] __attribute__((aligned(32))) = {
      255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,      255,      255,
      255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0xF0 - 1, 0xE0 - 1, 0xC0 - 1,
  };
  __m256i max_val = _mm256_load_si256((const __m256i*)max_arr);
  // unsigned >: input > max iff max(input, max) == input AND input != max
  __m256i ge = _mm256_cmpeq_epi8(_mm256_max_epu8(input, max_val), input);
  __m256i eq = _mm256_cmpeq_epi8(input, max_val);
  return _mm256_andnot_si256(eq, ge);
}

static __m256i _detect_leads(__m256i input) {
  __m256i shifted = _mm256_and_si256(_mm256_srli_epi16(input, 6), _mm256_set1_epi8(0x03));
  __m256i is_cont = _mm256_cmpeq_epi8(shifted, _mm256_set1_epi8(0x02));
  // Invert: NOT is_cont
  return _mm256_xor_si256(is_cont, _mm256_set1_epi8(-1));
}

static void _extract_marks(__m256i is_lead, uint8_t* out) {
  // _mm256_movemask_epi8 extracts the high bit of each byte -> 32-bit mask
  uint32_t mask = (uint32_t)_mm256_movemask_epi8(is_lead);
  memcpy(out, &mask, 4);
}

int ustr_validate(const uint8_t* data, size_t sz, uint8_t* marks) {
  __m256i error = _mm256_setzero_si256();
  __m256i prev_input = _mm256_setzero_si256();
  __m256i prev_incomplete = _mm256_setzero_si256();

  size_t i = 0;

  for (; i + 32 <= sz; i += 32) {
    __m256i input = _mm256_loadu_si256((const __m256i*)(data + i));

    // ASCII fast path: all bytes < 0x80
    __m256i high_bits = _mm256_and_si256(input, _mm256_set1_epi8((char)0x80));
    if (_mm256_testz_si256(high_bits, high_bits)) {
      error = _mm256_or_si256(error, prev_incomplete);
      prev_incomplete = _mm256_setzero_si256();
      prev_input = input;
      marks[i / 8 + 0] = 0xFF;
      marks[i / 8 + 1] = 0xFF;
      marks[i / 8 + 2] = 0xFF;
      marks[i / 8 + 3] = 0xFF;
      continue;
    }

    // Build combined register for cross-chunk lookback:
    // low lane = prev_input[16..31], high lane = input[0..15]
    __m256i prev_combined = _mm256_permute2x128_si256(prev_input, input, 0x21);

    __m256i prev1 = _prev_n(prev_combined, input, 1);
    __m256i sc = _check_special_cases(input, prev1);
    error = _mm256_or_si256(error, _check_multibyte_lengths(input, prev_combined, sc));

    prev_incomplete = _is_incomplete(input);
    prev_input = input;

    __m256i is_lead = _detect_leads(input);
    _extract_marks(is_lead, &marks[i / 8]);
  }

  // Only an error if there's no tail data to complete the sequence
  if (i >= sz) {
    error = _mm256_or_si256(error, prev_incomplete);
  }

  if (!_mm256_testz_si256(error, error)) {
    return -1;
  }

  // Scalar tail: back up to find any straddling multi-byte lead
  if (i < sz) {
    size_t backup = 0;
    for (size_t k = 1; k <= 3 && k <= i; k++) {
      uint8_t b = data[i - k];
      if (b >= 0xC0) {
        backup = k;
        break;
      }
      if (b < 0x80) {
        break;
      }
    }

    size_t start = i - backup;
    for (size_t j = start; j < sz; j++) {
      marks[j / 8] &= ~(uint8_t)(1u << (j % 8));
    }

    size_t tail_sz = sz - start;
    uint8_t tail_marks[8] = {0};
    if (ustr_validate_scalar(data + start, tail_sz, tail_marks) != 0) {
      return -1;
    }

    for (size_t j = 0; j < tail_sz; j++) {
      if (tail_marks[j / 8] & (1u << (j % 8))) {
        size_t abs_j = start + j;
        marks[abs_j / 8] |= (uint8_t)(1u << (abs_j % 8));
      }
    }
  }

  return 0;
}

#endif // __AVX2__
