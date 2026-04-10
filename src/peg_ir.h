// specs/peg_ir.md
#pragma once

#include "irwriter.h"
#include "peg.h"
#include "symtab.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  IrWriter* w;
  IrVal tokens;       // ptr to Token array (TokenChunk->tokens)
  IrVal col_index;    // alloca'd i32: current column index
  IrLabel fail_label; // current failure destination label
  IrVal stack;        // alloca'd ptr: stack pointer
  IrVal stack_bp;     // alloca'd ptr: base pointer for calls
  IrVal ret_val;      // alloca'd i32: return value register
  IrVal fast_ret;     // alloca'd ptr: fast return address for term calls
  IrVal table;        // ptr to memoize table base
  IrVal n_tokens;     // i64: total token count in scope (sext'd from i32 at scope entry)

  const char* scope_name;
  Symtab* scoped_rule_names;
  ScopedRule* rules; // darray of ScopedRule for this scope
  bool compress;     // row_shared mode

  int32_t col_sizeof;   // bytes per Col struct
  int32_t bits_offset;  // byte offset of bits[] in Col (0)
  int32_t slots_offset; // byte offset of slots[] in Col
  int32_t n_seg_groups; // number of segment groups (row_shared)
  int32_t n_slots;      // number of slots per Col
} PegIrCtx;

// emit IR for a terminal match. returns IrVal = consumed count (always 1 on success)
IrVal peg_ir_term(PegIrCtx* ctx, int32_t term_id);

// emit IR for a sub-rule call (pushes return address). returns IrVal = consumed count
IrVal peg_ir_call(PegIrCtx* ctx, int32_t scoped_rule_id);

// emit IR for a fast call to a non-memoizable rule (term). no stack push.
// stores blockaddress return label in fast_ret alloca, branches to target.
// returns IrVal = consumed count after target returns via indirectbr.
IrVal peg_ir_fast_call(PegIrCtx* ctx, int32_t scoped_rule_id);

// emit IR for a sequence of sub-rules. returns IrVal = total consumed
IrVal peg_ir_seq(PegIrCtx* ctx, int32_t* seq);

// emit IR for ordered choice. returns IrVal = consumed count of chosen branch
IrVal peg_ir_choice(PegIrCtx* ctx, int32_t* branches);

// emit IR for optional (?). returns IrVal = 0 or consumed
IrVal peg_ir_maybe(PegIrCtx* ctx, int32_t scoped_rule_id);

// emit IR for zero-or-more (*). returns IrVal = total consumed
IrVal peg_ir_star(PegIrCtx* ctx, int32_t scoped_rule_id, int32_t rhs_scoped_rule_id);

// emit IR for one-or-more (+). returns IrVal = total consumed
IrVal peg_ir_plus(PegIrCtx* ctx, int32_t scoped_rule_id, int32_t rhs_scoped_rule_id);

// memoize table access: read slot value
IrVal peg_ir_read_slot(PegIrCtx* ctx, IrVal col, uint32_t slot_index);

// memoize table access: write slot value
void peg_ir_write_slot(PegIrCtx* ctx, IrVal col, uint32_t slot_index, IrVal val);

// row_shared: test rule bit (returns i1: 1=may match, 0=proven fail)
IrVal peg_ir_bit_test(PegIrCtx* ctx, IrVal col, uint32_t seg_idx, uint32_t rule_bit);

// row_shared: deny rule bit (cache failure)
void peg_ir_bit_deny(PegIrCtx* ctx, IrVal col, uint32_t seg_idx, uint32_t rule_bit);

// row_shared: exclude other bits in segment (cache exclusive match)
void peg_ir_bit_exclude(PegIrCtx* ctx, IrVal col, uint32_t seg_idx, uint32_t rule_bit);

// emit shared internal LLVM helper function definitions (call once per module)
void peg_ir_emit_helpers(IrWriter* w);
