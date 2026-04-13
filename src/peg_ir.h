// specs/peg_ir.md
#pragma once

#include "irwriter.h"
#include "peg.h"

#include <stdint.h>

typedef struct {
  IrWriter* ir_writer;

  int64_t tag_bit_offset;
  const char* fn_name; // enclosing function name for blockaddress

  // shared registers
  IrVal tc;
  IrVal tokens;

  // shared allocas
  IrVal col;
  IrVal stack_ptr;
  IrVal parse_result;
  IrVal tag_bits;
  IrVal parsed_tokens;
  IrVal n_tokens;

  // accumulated return labels for indirectbr destination list
  IrLabel* ret_labels; // darray, owned
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
