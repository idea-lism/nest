// specs/peg_analyze.md, specs/peg_gen.md
#pragma once

#include "bitset.h"
#include "header_writer.h"
#include "irwriter.h"
#include "symtab.h"
#include "token_tree.h"

#include <stdbool.h>
#include <stdint.h>

// --- PEG input types (produced by parser, consumed by peg_analyze) ---

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
  int32_t scope_id;    // -1 for non-scope
  int32_t source_line; // line in .nest file (for LLVM IR debug info), 0 = unknown
  int32_t source_col;
  PegUnit body;
} PegRule;

typedef PegRule* PegRules; // darray

typedef struct {
  PegRules rules;
  Symtab tokens;      // owned by ParseState
  Symtab scope_names; // owned by ParseState
  Symtab rule_names;  // owned by ParseState
  int verbose;
} PegAnalyzeInput;

// --- Scoped unit types (produced by analysis) ---

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

// --- Node field layout (precomputed by analysis for codegen) ---

typedef enum {
  NODE_ADVANCE_NONE = 0, // link field, no cursor advance
  NODE_ADVANCE_ONE,      // term, advance cursor by 1
  NODE_ADVANCE_SLOT,     // call/branches, advance cursor by slot value
} NodeAdvanceKind;

typedef struct {
  char* name;               // sanitized, deduped (owned)
  bool is_link;             // PegLink vs PegRef
  bool is_scope;            // term refers to a scope
  int32_t ref_row;          // PegRef.row (physical slot row)
  int32_t rhs_row;          // PegLink interlace rhs row (-1 for none)
  NodeAdvanceKind advance;  // how to advance cursor after this field
  int32_t advance_slot_row; // valid when advance == NODE_ADVANCE_SLOT
  const char* wrapper_name; // for is_link: scoped_rule_name of wrapper (not owned)
} NodeField;
typedef NodeField* NodeFields; // darray

typedef struct {
  int32_t caller_id; // scoped_rule index of the caller, -1 for entrance
  int32_t site;      // nth call site within that caller (0-based)
} CallSite;

typedef struct {
  const char* scoped_rule_name; // not owned
  ScopedUnit body;              // tree clone
  Symtab tags;                  // total tags for this rule
  int32_t original_global_id;   // -1 for generated sub-rules

  // node field layout (precomputed, NULL for generated sub-rules)
  NodeFields node_fields;

  // tag bit allocation
  int32_t tag_bit_index;  // starting bucket index in bits[]
  int32_t tag_bit_offset; // bit offset within starting bucket (0..63)
  int32_t tag_bit_count;  // total number of tag bits for this rule

  // analysis
  bool nullable;
  Bitset* first_set;
  Bitset* last_set;
  uint32_t min_size;
  uint32_t max_size; // UINT32_MAX = unlimited

  // shared-mode slot coloring
  int32_t segment_color;  // logical color group id
  uint64_t segment_index; // physical bucket index in bits[]
  uint64_t segment_mask;
  uint64_t rule_bit_mask;
  uint64_t slot_index;

  // call site analysis (computed by peg_analyze)
  CallSite* call_sites; // darray
} ScopedRule;

typedef ScopedRule* ScopedRules; // darray

typedef struct {
  const char* scope_name;
  int32_t scope_id;
  int32_t source_line; // line in .nest file (from PegRule), for LLVM IR debug info
  int32_t source_col;
  Symtab scoped_rule_names;
  ScopedRules scoped_rules;

  int64_t bits_bucket_size;
  int64_t slots_size;
} ScopeClosure;

// --- Memoize modes (from cli.md) ---

typedef enum { MEMOIZE_NONE = 0, MEMOIZE_NAIVE = 1, MEMOIZE_SHARED = 2 } MemoizeMode;

// --- PEG codegen input (produced by peg_analyze, consumed by peg_gen) ---

typedef struct {
  ScopeClosure* scope_closures; // darray, fully analyzed (from peg_analyze)
  Symtab tokens;                // forwarded from PegAnalyzeInput
  Symtab scope_names;           // forwarded from PegAnalyzeInput
  Symtab rule_names;            // forwarded from PegAnalyzeInput
  int memoize_mode;
  const char* prefix;
  int verbose;
} PegGenInput;

// --- Public API ---

PegGenInput peg_analyze(PegAnalyzeInput* input, int memoize_mode, const char* prefix);
void peg_analyze_free(PegGenInput* result);
void peg_gen(PegGenInput* input, HeaderWriter* hw, IrWriter* w);
