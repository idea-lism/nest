#pragma once

#include <stdint.h>
#include <stdio.h>

typedef struct Lex Lex;

#define LEX_ERR_PAREN (-1)
#define LEX_ERR_BRACKET (-2)

Lex* lex_new(const char* source_file_name);
void lex_del(Lex* l);
int32_t lex_add(Lex* l, const char* mode, const char* pattern, int32_t source_file_offset);
void lex_gen(Lex* l, FILE* f, const char* target_triple);
