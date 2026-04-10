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
  SCOPED_RULE_KIND_BRANCHES,
  SCOPED_RULE_KIND_SEQ,
  SCOPED_RULE_KIND_CALL,
  SCOPED_RULE_KIND_JOIN,
  SCOPED_RULE_KIND_TERM,
} ScopedRuleKind;

typedef struct {
  const char* name;
  ScopedRuleKind kind;
  union {
    int32_t* branches; // darray of scoped_rule_ids
    int32_t* seq;      // darray of scoped_rule_ids
    int32_t call;      // scoped_rule_id
    struct {
      int32_t lhs;
      int32_t rhs;
    } join;
    int32_t term; // token id | scope id
  } as;
  char multiplier; // ?, *, +

  bool nullable;
  Bitset* first_set;
  Bitset* last_set;

  bool needs_memo; // non-term rules

  uint32_t scoped_rule_id;
  uint32_t segment_index;
  uint32_t segment_mask;
  uint32_t rule_bit_mask;
  uint32_t slot_index;
} ScopedRule;

typedef struct {
  const char* scope_name;
  int32_t scope_id;
  Symtab defined_rules;
  ScopedRule* rules; // darray
  int32_t* root_ids; // darray: root_ids[symtab_id] = ScopedRule index of that rule's root
  int32_t memoizable_size;
} ScopeClosure;

// --- Public API ---

void peg_gen(PegGenInput* input, HeaderWriter* hw, IrWriter* w, bool compress_memoize, const char* prefix);
