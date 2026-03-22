#include "header_writer.h"

#include <stdarg.h>
#include <stdlib.h>

struct HeaderWriter {
  FILE* out;
  int32_t indent;
};

HeaderWriter* hw_new(FILE* out) {
  HeaderWriter* hw = calloc(1, sizeof(HeaderWriter));
  hw->out = out;
  return hw;
}

void hw_del(HeaderWriter* hw) {
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

void hw_pragma_once(HeaderWriter* hw) { fputs("#pragma once\n", hw->out); }

void hw_include(HeaderWriter* hw, const char* header) { fprintf(hw->out, "#include \"%s\"\n", header); }

void hw_include_sys(HeaderWriter* hw, const char* header) { fprintf(hw->out, "#include <%s>\n", header); }

void hw_blank(HeaderWriter* hw) { fputc('\n', hw->out); }

void hw_comment(HeaderWriter* hw, const char* text) {
  _indent(hw);
  fprintf(hw->out, "// %s\n", text);
}

void hw_define(HeaderWriter* hw, const char* name, int32_t value) { fprintf(hw->out, "#define %s %d\n", name, value); }

void hw_define_str(HeaderWriter* hw, const char* name, const char* value) {
  fprintf(hw->out, "#define %s %s\n", name, value);
}

void hw_raw(HeaderWriter* hw, const char* text) { fputs(text, hw->out); }

void hw_fmt(HeaderWriter* hw, const char* fmt, ...) {
  _indent(hw);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(hw->out, fmt, ap);
  va_end(ap);
}

void hw_struct_begin(HeaderWriter* hw, const char* name) {
  _indent(hw);
  fprintf(hw->out, "typedef struct {\n");
  hw->indent++;
  (void)name;
}

void hw_struct_end(HeaderWriter* hw) {
  hw->indent--;
  _indent(hw);
  fputs("}", hw->out);
}

void hw_field(HeaderWriter* hw, const char* type, const char* name) {
  _indent(hw);
  fprintf(hw->out, "%s %s;\n", type, name);
}

void hw_bitfield(HeaderWriter* hw, const char* type, const char* name, int32_t bits) {
  _indent(hw);
  fprintf(hw->out, "%s %s : %d;\n", type, name, bits);
}

void hw_typedef(HeaderWriter* hw, const char* type, const char* name) {
  fprintf(hw->out, "typedef %s %s;\n", type, name);
}

void hw_enum_begin(HeaderWriter* hw, const char* name) {
  fprintf(hw->out, "enum %s {\n", name);
  hw->indent++;
}

void hw_enum_value(HeaderWriter* hw, const char* name, int32_t value) {
  _indent(hw);
  fprintf(hw->out, "%s = %d,\n", name, value);
}

void hw_enum_end(HeaderWriter* hw) {
  hw->indent--;
  fputs("};\n", hw->out);
}
