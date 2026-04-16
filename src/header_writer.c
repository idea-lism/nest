#include "header_writer.h"

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>

struct HeaderWriter {
  FILE* out;
  int32_t indent;
};

HeaderWriter* hdwriter_new(FILE* out) {
  HeaderWriter* hw = calloc(1, sizeof(HeaderWriter));
  hw->out = out;
  return hw;
}

void hdwriter_del(HeaderWriter* hw) {
  if (!hw) {
    return;
  }
  free(hw);
}

static void _indent(HeaderWriter* hw) {
  for (int32_t i = 0; i < hw->indent; i++) {
    fputs("  ", hw->out);
  }
}

void hdwriter_puts(HeaderWriter* hw, const char* text) {
  _indent(hw);
  fputs(text, hw->out);
}

void hdwriter_putc(HeaderWriter* hw, char c) { fputc(c, hw->out); }

void hdwriter_printf(HeaderWriter* hw, const char* fmt, ...) {
  _indent(hw);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(hw->out, fmt, ap);
  va_end(ap);
}

void hdwriter_begin(HeaderWriter* hw) {
  fputs(" {\n", hw->out);
  hw->indent++;
}

void hdwriter_end(HeaderWriter* hw) {
  hw->indent--;
  assert(hw->indent >= 0);
  _indent(hw);
  fputs("}\n", hw->out);
}
