#pragma once

// Internal interfaces for testing and benchmarking.
// Not part of the public API -- do not include from user code.

#include "ustr.h"

// Scalar UTF-8 validator (always available).
int ustr_validate_scalar(const uint8_t* data, size_t sz, uint8_t* marks);

// Primary validator (NEON on aarch64, scalar fallback otherwise).
int ustr_validate(const uint8_t* data, size_t sz, uint8_t* marks);

// Naive (bit-by-bit) versions of size/slice for benchmarking.
int32_t ustr_size_naive(const char* s);
char* ustr_slice_naive(const char* s, int32_t start, int32_t end);
