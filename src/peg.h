#pragma once

#include "bitset.h"
#include "header_writer.h"
#include "irwriter.h"
#include "symtab.h"

#include <stdbool.h>
#include <stdint.h>

// --- Parser-produced PEG tree (input to peg_gen) ---

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

// --- Broken-down scoped rules (output of rule breakdown) ---

typedef struct {
  int32_t source_file_line;
  int32_t source_file_col;
} PegDebugInfo;

typedef enum {
  SCOPED_RULE_KIND_BRANCHES,
  SCOPED_RULE_KIND_SEQ,
  SCOPED_RULE_KIND_CALL,
  SCOPED_RULE_KIND_JOIN,
  SCOPED_RULE_KIND_TOK,
  SCOPED_RULE_KIND_EXTERNAL_SCOPE,
} ScopedRuleKind;

typedef struct {
  const char* name; // e.g. "foo$1$2"
  ScopedRuleKind kind;
  union {
    int32_t* branches; // darray of scoped_rule_ids
    int32_t* seq;      // darray of scoped_rule_ids
    int32_t call;      // scoped_rule_id
    struct {
      int32_t lhs, rhs;
    } join;                 // lhs*<rhs> / lhs+<rhs>
    int32_t tok;            // token id
    int32_t external_scope; // global rule index
  } as;
  char multiplier; // '?', '*', '+', or 0
  PegDebugInfo di;

  // analysis fields (populated by _analyze_closure):
  bool nullable;     // can the rule match 0-length input?
  Bitset* first_set; // tokens that can start a match (excluding epsilon)
  Bitset* last_set;  // tokens that can end a match (excluding epsilon)

  // per-scope codegen results (populated by peg_gen):
  uint32_t scoped_rule_id; // unique in a scope closure
  uint32_t segment_index;  // check Col.bits[segment_index]
  uint32_t segment_mask;   // OR of all rule_bit_masks sharing this segment
  uint32_t rule_bit_mask;  // single bit in segment_mask
  uint32_t slot_index;     // Col.slots[slot_index]
} ScopedRule;

typedef struct {
  const char* scope_name;
  Symtab defined_rules; // maps rule name -> scoped_rule_id
  ScopedRule* rules;    // darray of ScopedRule

  // populated during analysis/codegen:
  int32_t n_slots;
  int32_t n_bits;
  char col_type[64];     // LLVM IR type name (e.g. "Col.main")
  char hdr_col_type[64]; // C header type name (e.g. "Col_main")
} ScopeClosure;

void peg_gen(PegGenInput* input, HeaderWriter* hw, IrWriter* w, bool compress_memoize, const char* prefix);
