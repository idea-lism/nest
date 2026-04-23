// specs/symtab.md
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
  char* buf;         // darray of char — all strings concatenated, '\0'-separated
  int32_t* offsets;  // darray of int32_t — byte offset of each string in buf
  int32_t start_num; // first ID returned (IDs are start_num, start_num+1, ...)
} Symtab;

void symtab_init(Symtab* st, int32_t start_num);
void symtab_free(Symtab* st);
int32_t symtab_intern(Symtab* st, const char* name);
int32_t symtab_intern_f(Symtab* st, const char* fmt_str, ...);
int32_t symtab_find(const Symtab* st, const char* name);
int32_t symtab_find_f(const Symtab* st, const char* fmt_str, ...);
const char* symtab_get(const Symtab* st, int32_t id);
size_t symtab_count(const Symtab* st);
