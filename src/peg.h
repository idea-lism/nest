#pragma once

#include "header_writer.h"
#include "irwriter.h"

#include <stdbool.h>
#include <stdint.h>

// PEG unit kinds
typedef enum {
  PEG_ID,
  PEG_TOK,
  PEG_KEYWORD_TOK,
  PEG_BRANCHES,
  PEG_SEQ,
} PegUnitKind;

typedef struct PegUnit PegUnit;

struct PegUnit {
  PegUnitKind kind;
  char* name;         // (owned, may be NULL)
  int32_t multiplier; // '?','+','*', or 0
  PegUnit* interlace;
  int32_t ninterlace;
  char* tag;         // (owned, may be NULL)
  PegUnit* children; // darray
};

typedef struct {
  char* name; // (owned)
  PegUnit seq;
  char* scope; // (owned, may be NULL) - if NULL, uses "main"
} PegRule;

typedef struct {
  PegRule* rules;
} PegGenInput;

void peg_gen(PegGenInput* input, HeaderWriter* hw, IrWriter* w, bool compress_memoize);
