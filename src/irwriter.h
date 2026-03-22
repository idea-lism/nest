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

void irwriter_bb(IrWriter* w, const char* label);

void irwriter_dbg(IrWriter* w, int32_t line, int32_t col);

int32_t irwriter_binop(IrWriter* w, char* buf, int32_t buf_size, const char* op, const char* ty, const char* lhs,
                       const char* rhs);
int32_t irwriter_binop_imm(IrWriter* w, char* buf, int32_t buf_size, const char* op, const char* ty, const char* lhs,
                           int64_t rhs);

int32_t irwriter_icmp(IrWriter* w, char* buf, int32_t buf_size, const char* pred, const char* ty, const char* lhs,
                      const char* rhs);
int32_t irwriter_icmp_imm(IrWriter* w, char* buf, int32_t buf_size, const char* pred, const char* ty, const char* lhs,
                          int64_t rhs);

void irwriter_br(IrWriter* w, const char* label);
void irwriter_br_cond(IrWriter* w, const char* cond, const char* if_true, const char* if_false);

void irwriter_switch_start(IrWriter* w, const char* ty, const char* val, const char* default_label);
void irwriter_switch_case(IrWriter* w, const char* ty, int64_t val, const char* label);
void irwriter_switch_end(IrWriter* w);

void irwriter_ret(IrWriter* w, const char* ty, const char* val);

int32_t irwriter_insertvalue(IrWriter* w, char* buf, int32_t buf_size, const char* agg_ty, const char* agg_val,
                             const char* elem_ty, const char* elem_val, int idx);
int32_t irwriter_insertvalue_imm(IrWriter* w, char* buf, int32_t buf_size, const char* agg_ty, const char* agg_val,
                                 const char* elem_ty, int64_t elem_val, int idx);

void irwriter_declare(IrWriter* w, const char* ret_type, const char* name, const char* arg_types);

void irwriter_call_void(IrWriter* w, const char* name);
