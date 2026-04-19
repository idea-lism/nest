#ifdef __aarch64__

#include "ustr.h"
#include "ustr_intern.h"
#include <arm_neon.h>
#include <string.h>

// NEON-accelerated UTF-8 validation with simultaneous marks bitset generation.
//
// Based on the simdutf "lookup4" algorithm:
//   - 3 table lookups (vqtbl1q_u8) classify pairs of adjacent bytes
//   - AND the 3 results: any nonzero bit = error
//   - Additionally check that continuation bytes appear where expected
//
// Marks generation:
//   - A byte is a codepoint start iff it is NOT a continuation byte (10xxxxxx)
//   - Detect via (byte >> 6) == 2, invert, then compress 16 lanes to 16 bits
//     using AND with positional bit mask + pairwise add cascade (vpaddq_u8)

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

static const uint8_t bit_tbl_data[16] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
};

static uint8x16_t _check_special_cases(uint8x16_t input, uint8x16_t prev1) {
  uint8x16_t b1h_tbl = vld1q_u8(byte_1_high_tbl);
  uint8x16_t b1l_tbl = vld1q_u8(byte_1_low_tbl);
  uint8x16_t b2h_tbl = vld1q_u8(byte_2_high_tbl);

  uint8x16_t prev1_hi = vshrq_n_u8(prev1, 4);
  uint8x16_t prev1_lo = vandq_u8(prev1, vmovq_n_u8(0x0F));
  uint8x16_t cur_hi = vshrq_n_u8(input, 4);

  uint8x16_t byte_1_high = vqtbl1q_u8(b1h_tbl, prev1_hi);
  uint8x16_t byte_1_low = vqtbl1q_u8(b1l_tbl, prev1_lo);
  uint8x16_t byte_2_high = vqtbl1q_u8(b2h_tbl, cur_hi);

  return vandq_u8(vandq_u8(byte_1_high, byte_1_low), byte_2_high);
}

static uint8x16_t _check_multibyte_lengths(uint8x16_t input, uint8x16_t prev_input, uint8x16_t sc) {
  uint8x16_t prev2 = vextq_u8(prev_input, input, 16 - 2);
  uint8x16_t prev3 = vextq_u8(prev_input, input, 16 - 3);

  uint8x16_t is_third = vcgeq_u8(prev2, vmovq_n_u8(0xE0));
  uint8x16_t is_fourth = vcgeq_u8(prev3, vmovq_n_u8(0xF0));

  uint8x16_t must23 = veorq_u8(is_third, is_fourth);
  uint8x16_t must23_80 = vandq_u8(must23, vmovq_n_u8(0x80));
  return veorq_u8(must23_80, sc);
}

static uint8x16_t _is_incomplete(uint8x16_t input) {
  static const uint8_t max_arr[16] = {255, 255, 255, 255, 255, 255,      255,      255,
                                      255, 255, 255, 255, 255, 0xF0 - 1, 0xE0 - 1, 0xC0 - 1};
  uint8x16_t max_val = vld1q_u8(max_arr);
  return vcgtq_u8(input, max_val);
}

// Compress 16 lanes of 0x00/0xFF to a 16-bit mask, stored as 2 bytes
static void _extract_marks(uint8x16_t is_lead, uint8_t* out) {
  uint8x16_t bit_mask = vld1q_u8(bit_tbl_data);
  uint8x16_t bits = vandq_u8(is_lead, bit_mask);
  uint8x16_t p0 = vpaddq_u8(bits, bits);
  uint8x16_t p1 = vpaddq_u8(p0, p0);
  uint8x16_t p2 = vpaddq_u8(p1, p1);
  uint16_t mark_bits = vgetq_lane_u16(vreinterpretq_u16_u8(p2), 0);
  memcpy(out, &mark_bits, 2);
}

// NOT a continuation byte (10xxxxxx)
static uint8x16_t _detect_leads(uint8x16_t input) {
  uint8x16_t shifted = vshrq_n_u8(input, 6);
  uint8x16_t is_cont = vceqq_u8(shifted, vmovq_n_u8(0x02));
  return vmvnq_u8(is_cont);
}

int ustr_validate(const uint8_t* data, size_t sz, uint8_t* marks) {
  uint8x16_t error = vmovq_n_u8(0);
  uint8x16_t prev_input = vmovq_n_u8(0);
  uint8x16_t prev_incomplete = vmovq_n_u8(0);

  size_t i = 0;

  for (; i + 16 <= sz; i += 16) {
    uint8x16_t input = vld1q_u8(data + i);

    // ASCII fast path
    uint8x16_t high_bits = vandq_u8(input, vmovq_n_u8(0x80));
    if (vmaxvq_u8(high_bits) == 0) {
      error = vorrq_u8(error, prev_incomplete);
      prev_incomplete = vmovq_n_u8(0);
      prev_input = input;
      marks[i / 8 + 0] = 0xFF;
      marks[i / 8 + 1] = 0xFF;
      continue;
    }

    uint8x16_t prev1 = vextq_u8(prev_input, input, 16 - 1);
    uint8x16_t sc = _check_special_cases(input, prev1);
    error = vorrq_u8(error, _check_multibyte_lengths(input, prev_input, sc));

    prev_incomplete = _is_incomplete(input);
    prev_input = input;

    uint8x16_t is_lead = _detect_leads(input);
    _extract_marks(is_lead, &marks[i / 8]);
  }

  // Only an error if there's no tail data to complete the sequence
  if (i >= sz) {
    error = vorrq_u8(error, prev_incomplete);
  }

  if (vmaxvq_u8(error) != 0) {
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
    uint8_t tail_marks[4] = {0};
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

#endif // __aarch64__
