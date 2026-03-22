#pragma once

#include <stdint.h>
#include <stdio.h>

typedef struct HeaderWriter HeaderWriter;

HeaderWriter* hw_new(FILE* out);
void hw_del(HeaderWriter* hw);

void hw_pragma_once(HeaderWriter* hw);
void hw_include(HeaderWriter* hw, const char* header);
void hw_include_sys(HeaderWriter* hw, const char* header);
void hw_blank(HeaderWriter* hw);
void hw_comment(HeaderWriter* hw, const char* text);
void hw_define(HeaderWriter* hw, const char* name, int32_t value);
void hw_define_str(HeaderWriter* hw, const char* name, const char* value);
void hw_raw(HeaderWriter* hw, const char* text);
void hw_fmt(HeaderWriter* hw, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

void hw_struct_begin(HeaderWriter* hw, const char* name);
void hw_struct_end(HeaderWriter* hw);
void hw_field(HeaderWriter* hw, const char* type, const char* name);
void hw_bitfield(HeaderWriter* hw, const char* type, const char* name, int32_t bits);

void hw_typedef(HeaderWriter* hw, const char* type, const char* name);
void hw_enum_begin(HeaderWriter* hw, const char* name);
void hw_enum_value(HeaderWriter* hw, const char* name, int32_t value);
void hw_enum_end(HeaderWriter* hw);
