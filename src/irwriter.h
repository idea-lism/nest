#pragma once

#include <stdint.h>
#include <stdio.h>

typedef struct IrWriter IrWriter;

IrWriter* irwriter_new(FILE* out, const char* target_triple);
void irwriter_del(IrWriter* w);

void irwriter_start(IrWriter* w, const char* source_file, const char* directory);
void irwriter_end(IrWriter* w);

void irwriter_define_start(IrWriter* w, const char* name, const char* ret_type, int argc, const char** arg_types,
                           const char** arg_names);
void irwriter_define_end(IrWriter* w);

// Labels are integer ids. Reserve a label id without emitting a BB:
int32_t irwriter_label(IrWriter* w);
// Emit a basic block header (L<id>:). Returns the label id.
int32_t irwriter_bb(IrWriter* w);
// Emit a basic block for a pre-reserved label.
void irwriter_bb_at(IrWriter* w, int32_t label);

void irwriter_dbg(IrWriter* w, int32_t line, int32_t col);

int32_t irwriter_imm(IrWriter* w, const char* ty, int64_t val);
int32_t irwriter_param(IrWriter* w, const char* ty, const char* name);
int32_t irwriter_binop(IrWriter* w, const char* op, const char* ty, int32_t lhs_reg, int32_t rhs_reg);

int32_t irwriter_icmp(IrWriter* w, const char* pred, const char* ty, int32_t lhs_reg, int32_t rhs_reg);
int32_t irwriter_icmp_imm(IrWriter* w, const char* pred, const char* ty, int32_t lhs_reg, int64_t rhs);

void irwriter_br(IrWriter* w, int32_t label);
void irwriter_br_cond(IrWriter* w, const char* cond, int32_t if_true, int32_t if_false);
void irwriter_br_cond_r(IrWriter* w, int32_t cond_reg, int32_t if_true, int32_t if_false);

void irwriter_switch_start(IrWriter* w, const char* ty, const char* val, int32_t default_label);
void irwriter_switch_case(IrWriter* w, const char* ty, int64_t val, int32_t label);
void irwriter_switch_end(IrWriter* w);

void irwriter_ret(IrWriter* w, const char* ty, int32_t reg);
void irwriter_ret_void(IrWriter* w);
void irwriter_ret_i(IrWriter* w, const char* ty, int64_t val);

int32_t irwriter_insertvalue(IrWriter* w, const char* agg_ty, int32_t agg_reg, const char* elem_ty, int32_t elem_reg,
                             int idx);

void irwriter_declare(IrWriter* w, const char* ret_type, const char* name, const char* arg_types);

void irwriter_call_void(IrWriter* w, const char* name);
void irwriter_call_void_fmt(IrWriter* w, const char* name, const char* args);
int32_t irwriter_call_ret(IrWriter* w, const char* ret_ty, const char* name, const char* args);

int32_t irwriter_alloca(IrWriter* w, const char* ty);
int32_t irwriter_load(IrWriter* w, const char* ty, int32_t ptr_reg);
void irwriter_store(IrWriter* w, const char* ty, int32_t val_reg, int32_t ptr_reg);
void irwriter_store_imm(IrWriter* w, const char* ty, int64_t val, int32_t ptr_reg);
int32_t irwriter_gep(IrWriter* w, const char* base_ty, int32_t ptr_reg, const char* indices);

int32_t irwriter_phi2(IrWriter* w, const char* ty, int32_t v1_reg, int32_t bb1, int32_t v2_reg, int32_t bb2);
int32_t irwriter_select(IrWriter* w, const char* cond, const char* ty, const char* true_val, const char* false_val);
int32_t irwriter_sext(IrWriter* w, const char* from_ty, const char* val, const char* to_ty);

void irwriter_type_def(IrWriter* w, const char* name, const char* body);

void irwriter_raw(IrWriter* w, const char* text);
