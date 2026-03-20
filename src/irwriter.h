#pragma once

#include <stdint.h>
#include <stdio.h>

typedef struct irwriter irwriter;

// Lifecycle
irwriter* irwriter_new(FILE* out);
void irwriter_del(irwriter* w);

// Module prelude/epilogue
void irwriter_start(irwriter* w, const char* source_filename, const char* target_triple);
void irwriter_end(irwriter* w);

// Function define
void irwriter_define_start(irwriter* w, const char* name, const char* ret_type, int argc, const char** arg_types,
                           const char** arg_names);
void irwriter_define_end(irwriter* w);

// Basic blocks
void irwriter_bb(irwriter* w, const char* label);

// Debug location (set before emitting instructions; NULL to clear)
void irwriter_dbg(irwriter* w, int32_t line, int32_t col);

// Instructions -- all return the SSA name (e.g. "%r3"), valid until next call.
// Caller does NOT free.

// Binary ops: add, sub, mul, sdiv, srem, and, or, xor
const char* irwriter_binop(irwriter* w, const char* op, const char* ty, const char* lhs, const char* rhs);
const char* irwriter_binop_imm(irwriter* w, const char* op, const char* ty, const char* lhs, int64_t rhs);

// Compare: eq, ne, slt, sle, sgt, sge, ult, ule, ugt, uge
const char* irwriter_icmp(irwriter* w, const char* pred, const char* ty, const char* lhs, const char* rhs);
const char* irwriter_icmp_imm(irwriter* w, const char* pred, const char* ty, const char* lhs, int64_t rhs);

// Branch
void irwriter_br(irwriter* w, const char* label);
void irwriter_br_cond(irwriter* w, const char* cond, const char* if_true, const char* if_false);

// Switch
void irwriter_switch_start(irwriter* w, const char* ty, const char* val, const char* default_label);
void irwriter_switch_case(irwriter* w, const char* ty, int64_t val, const char* label);
void irwriter_switch_end(irwriter* w);

// Return
void irwriter_ret(irwriter* w, const char* ty, const char* val);

// Aggregate (insertvalue)
const char* irwriter_insertvalue(irwriter* w, const char* agg_ty, const char* agg_val, const char* elem_ty,
                                 const char* elem_val, int idx);
const char* irwriter_insertvalue_imm(irwriter* w, const char* agg_ty, const char* agg_val, const char* elem_ty,
                                     int64_t elem_val, int idx);
