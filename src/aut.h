#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct Aut Aut;

typedef struct {
  int32_t from_state_id;
  int32_t to_state_id;
  int32_t cp_start;
  int32_t cp_end_inclusive;
} TransitionDef;

typedef struct {
  int32_t source_file_line;
  int32_t source_file_col;
} DebugInfo;

Aut* aut_new(void);
void aut_del(Aut* dfa);
void aut_transition(Aut* dfa, TransitionDef tdef, DebugInfo di);
void aut_epsilon(Aut* dfa, int32_t from_state, int32_t to_state);
void aut_action(Aut* dfa, int32_t state, int32_t action_id);
void aut_optimize(Aut* dfa);
int32_t aut_dfa_nstates(Aut* dfa);
