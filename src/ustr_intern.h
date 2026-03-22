#pragma once

#include "ustr.h"

int ustr_validate_scalar(const uint8_t* data, size_t sz, uint8_t* marks);

int ustr_validate(const uint8_t* data, size_t sz, uint8_t* marks);

int32_t ustr_size_naive(const char* s);
char* ustr_slice_naive(const char* s, int32_t start, int32_t end);
