// specs/peg.md
#pragma once

#include "bitset.h"
#include "header_writer.h"
#include "irwriter.h"
#include "symtab.h"
#include "token_tree.h"

#include <stdbool.h>
#include <stdint.h>

// --- PEG input types (produced by parser, consumed by peg_gen) ---

typedef enum {
  PEG_CALL = 1,
  PEG_TERM,
  PEG_BRANCHES,
  PEG_SEQ,
} PegUnitKind;

typedef struct PegUnit PegUnit;
typedef PegUnit* PegUnits; // darray

struct PegUnit {
  PegUnitKind kind;

  // PEG_CALL: callee rule's global_id
  // PEG_TERM: token id | scope id
  int32_t id;

  char multiplier;                // '?','+','*', or 0
  PegUnitKind interlace_rhs_kind; // 0 | PEG_CALL | PEG_TERM
  int32_t interlace_rhs_id;

  char* tag;         // (owned, may be NULL)
  PegUnits children; // darray
};

typedef struct {
  int32_t global_id;
  int32_t scope_id; // -1 for non-scope
  PegUnit body;
} PegRule;

typedef PegRule* PegRules; // darray

typedef struct {
  PegRules rules;
  Symtab tokens;      // owned by ParseState
  Symtab scope_names; // owned by ParseState
  Symtab rule_names;  // owned by ParseState
  int32_t verbose;
} PegGenInput;

// --- Internal types for code generation ---

typedef enum {
  SCOPED_UNIT_CALL = 1,
  SCOPED_UNIT_TERM,
  SCOPED_UNIT_BRANCHES,
  SCOPED_UNIT_SEQ,
  SCOPED_UNIT_MAYBE,
  SCOPED_UNIT_STAR,
  SCOPED_UNIT_PLUS,
} ScopedUnitKind;

typedef struct ScopedUnit ScopedUnit;
typedef ScopedUnit* ScopedUnits; // darray

typedef struct {
  ScopedUnit* lhs;
  ScopedUnit* rhs;
} ScopedInterlace;

struct ScopedUnit {
  ScopedUnitKind kind;

  union {
    const char* callee;        // scoped_rule_name from symtab, not owned
    int32_t term_id;           // token id | scope id
    ScopedUnits children;      // for branches & seq
    ScopedUnit* base;          // maybe
    ScopedInterlace interlace; // for star & plus
  } as;

  int32_t tag_bit_local_offset; // index in tags, or -1 for non-tagged

  // if the unit is nullable, we need advancement check for multiplier rules
  bool nullable;
};

typedef struct {
  const char* scoped_rule_name; // not owned
  ScopedUnit body;              // tree clone
  Symtab tags;                  // total tags for this rule
  int32_t original_global_id;   // -1 for generated sub-rules

  // tag bit allocation
  uint64_t tag_bit_index;
  uint64_t tag_bit_mask;
  uint64_t tag_bit_offset;

  // analysis
  bool nullable;
  Bitset* first_set;
  Bitset* last_set;
  uint32_t min_size;
  uint32_t max_size; // UINT32_MAX = unlimited

  // shared-mode slot coloring
  uint64_t segment_index;
  uint64_t segment_mask;
  uint64_t rule_bit_mask;
  uint64_t slot_index;
} ScopedRule;

typedef ScopedRule* ScopedRules; // darray

typedef struct {
  const char* scope_name;
  int32_t scope_id;
  Symtab scoped_rule_names;
  ScopedRules scoped_rules;

  int64_t bits_bucket_size;
  int64_t slots_size;
} ScopeClosure;

// --- Memoize modes (from cli.md) ---

typedef enum { MEMO_NONE = 0, MEMO_NAIVE = 1, MEMO_SHARED = 2 } MemoMode;

// --- Public API ---

void peg_gen(PegGenInput* input, HeaderWriter* hw, IrWriter* w, int memoize_mode, const char* prefix);
