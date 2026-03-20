#pragma once

#include "irwriter.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct Aut Aut;

typedef struct {
  int32_t from_state_id;
  int32_t to_state_id;
  int32_t cp_start;
  int32_t cp_end_inclusive;
  int32_t action_id;
} TransitionDef;

typedef struct {
  int32_t source_file_line;
  int32_t source_file_col;
} DebugInfo;

Aut* aut_new(const char* function_name, const char* source_file_name);
void aut_del(Aut* dfa);
void aut_transition(Aut* dfa, TransitionDef tdef, DebugInfo di);
void aut_epsilon(Aut* dfa, int32_t from_state, int32_t to_state);
void aut_optimize(Aut* dfa);
void aut_gen_dfa(Aut* dfa, IrWriter* writer, bool debug_mode);
