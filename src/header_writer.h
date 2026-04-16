#pragma once

#include <stdint.h>
#include <stdio.h>

typedef struct HeaderWriter HeaderWriter;

// Allocate + zero-init. Store out.
HeaderWriter* hdwriter_new(FILE* out);

// Free if non-NULL.
void hdwriter_del(HeaderWriter* hw);

// Emit indentation (2 spaces x indent), then fputs(text, out).
void hdwriter_puts(HeaderWriter* hw, const char* text);

// Emit single char via fputc. No indentation.
void hdwriter_putc(HeaderWriter* hw, char c);

// Emit indentation, then vfprintf.
void hdwriter_printf(HeaderWriter* hw, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Emit " {\n" (space, open brace, newline), then increment indent.
// Use for pure { } blocks (function bodies, switch, if). Not for structs/typedefs
// where the closing brace has a suffix (e.g. "} Name;").
void hdwriter_begin(HeaderWriter* hw);
void hdwriter_end(HeaderWriter* hw);
