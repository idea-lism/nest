// specs/peg_ir.md
#pragma once

#include "irwriter.h"
#include "peg.h"

#include <stdint.h>

typedef struct {
  IrWriter* ir_writer;

  int32_t tag_bit_offset;
  int32_t tag_bit_count;
  const char* fn_name; // enclosing function name for blockaddress

  // shared registers
  IrVal tc;
  IrVal tokens;

  // shared allocas
  IrVal col;
  IrVal stack_ptr;
  IrVal parse_result;
  IrVal tag_bits;        // first tag_bits alloca (or only one if tag_bits_n <= 1)
  IrVal* tag_bits_extra; // additional tag_bits allocas for multi-bucket (NULL if tag_bits_n <= 1)
  int32_t tag_bits_n;    // number of tag_bits allocas (max across all rules in scope)
  bool has_tags;         // whether current rule has tag bits (controls stack push/pop around calls)
  IrVal parsed_tokens;

  // call site tracking for indirectbr
  CallSite* current_rule_call_sites; // not owned, points to current rule's call_sites darray
  int32_t call_site_counter;         // local counter for current caller rule
  int32_t current_rule_id;           // scoped_rule index of the rule being emitted
} PegIrCtx;

// emit IR for a ScopedUnit tree; on success falls through with parsed_tokens set; on failure branches to fail_label
void peg_ir_emit_parse(PegIrCtx* ctx, ScopedUnit* unit, IrLabel fail_label);

// push ret_site + col onto stack, branch to callee_name. Returns the ret_label for post-return handling.
IrLabel peg_ir_emit_call(PegIrCtx* ctx, const char* callee_name);

// emit call-return epilogue (indirectbr through stack)
void peg_ir_emit_ret(PegIrCtx* ctx);

// emit helper function definitions: @save, @restore
void peg_ir_emit_helpers(IrWriter* w);

// emit shared-mode bit helper definitions: @bit_test, @bit_deny, @bit_exclude
void peg_ir_emit_bit_helpers(IrWriter* w);
// emit GEP + tag writeback helper definitions: @gep_slot, @gep_tag, @tag_writeback
void peg_ir_emit_gep_helpers(IrWriter* w);
