#pragma once

#include "aut.h"
#include <stdbool.h>
#include <stdint.h>

#define LEX_CP_BOF (-1)
#define LEX_CP_EOF (-2)

typedef struct Re Re;

typedef struct {
  int32_t start;
  int32_t end;
} ReInterval;

typedef struct {
  ReInterval* ivs;
  int32_t len;
  int32_t cap;
} ReRange;

ReRange* re_range_new(void);
void re_range_del(ReRange* range);
void re_range_add(ReRange* range, int32_t start_cp, int32_t end_cp);
void re_range_neg(ReRange* range);

Re* re_new(Aut* aut);
void re_del(Re* re);

void re_append_ch(Re* re, int32_t codepoint, DebugInfo di);
void re_append_range(Re* re, ReRange* range, DebugInfo di);
void re_lparen(Re* re);
void re_fork(Re* re);
void re_rparen(Re* re);
void re_action(Re* re, int32_t action_id);
int32_t re_cur_state(Re* re);
