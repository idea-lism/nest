#pragma once

#include "aut.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct Re Re;

typedef struct {
  int32_t start;
  int32_t end;
} ReInterval;

typedef struct {
  ReInterval* ivs; // darray
  bool negated;
} ReRange;

ReRange* re_range_new(void);
void re_range_del(ReRange* range);
void re_range_add(ReRange* range, int32_t start_cp, int32_t end_cp);
void re_range_neg(ReRange* range);
void re_range_ic(ReRange* range);

Re* re_new(Aut* aut);
void re_del(Re* re);

void re_append_ch(Re* re, int32_t codepoint, DebugInfo di);
void re_append_ch_ic(Re* re, int32_t codepoint, DebugInfo di);
void re_append_range(Re* re, ReRange* range, DebugInfo di);

void re_append_group_s(ReRange* range);
void re_append_group_d(ReRange* range);
void re_append_group_w(ReRange* range);
void re_append_group_h(ReRange* range);
void re_append_group_dot(ReRange* range);
int32_t re_c_escape(char symbol);
int32_t re_hex_to_codepoint(const char* h, size_t size);

void re_lparen(Re* re);
void re_fork(Re* re);
void re_rparen(Re* re);
void re_action(Re* re, int32_t action_id);
void re_loop_back(Re* re);
int32_t re_cur_state(Re* re);

typedef struct ReLex ReLex;
typedef struct IrWriter IrWriter;
ReLex* re_lex_new(const char* func_name, const char* source_file_name, const char* mode);
void re_lex_del(ReLex* l);
int32_t re_lex_add(ReLex* l, const char* pattern, int32_t line, int32_t col, int32_t action_id);
void re_lex_gen(ReLex* l, IrWriter* w, bool debug_mode);
