#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

typedef struct IrWriter IrWriter;
typedef int32_t IrVal;
typedef int32_t IrLabel;

IrWriter* irwriter_new(FILE* out, const char* target_triple);
void irwriter_del(IrWriter* w);

void irwriter_start(IrWriter* w, const char* source_file, const char* directory);
void irwriter_end(IrWriter* w);

void irwriter_define_start(IrWriter* w, const char* name, const char* ret_type, int argc, const char** arg_types,
                           const char** arg_names);
void irwriter_define_end(IrWriter* w);

IrLabel irwriter_label(IrWriter* w);
IrLabel irwriter_label_f(IrWriter* w, const char* fmt, ...);
IrLabel irwriter_bb(IrWriter* w);
void irwriter_bb_at(IrWriter* w, IrLabel label);

void irwriter_dbg(IrWriter* w, int32_t line, int32_t col);

IrVal irwriter_imm(IrWriter* w, const char* literal);
IrVal irwriter_imm_int(IrWriter* w, int v);
IrVal irwriter_binop(IrWriter* w, const char* op, const char* ty, IrVal lhs, IrVal rhs);

IrVal irwriter_icmp(IrWriter* w, const char* pred, const char* ty, IrVal lhs, IrVal rhs);

void irwriter_br(IrWriter* w, IrLabel label);
void irwriter_br_cond(IrWriter* w, IrVal cond, IrLabel if_true, IrLabel if_false);

void irwriter_switch_start(IrWriter* w, const char* ty, IrVal val, IrLabel default_label);
void irwriter_switch_case(IrWriter* w, const char* ty, int64_t val, IrLabel label);
void irwriter_switch_end(IrWriter* w);

void irwriter_ret(IrWriter* w, const char* ty, IrVal val);
void irwriter_ret_void(IrWriter* w);

IrVal irwriter_insertvalue(IrWriter* w, const char* agg_ty, IrVal agg, const char* elem_ty, IrVal elem, int idx);
IrVal irwriter_extractvalue(IrWriter* w, const char* agg_ty, IrVal agg, int idx);

// declare function, handles repeated decl
void irwriter_declare(IrWriter* w, const char* ret_type, const char* name, const char* arg_types);

void irwriter_call_void_fmtf(IrWriter* w, const char* name, const char* args_fmt, ...)
    __attribute__((format(printf, 3, 4)));
IrVal irwriter_call_retf(IrWriter* w, const char* ret_ty, const char* name, const char* args_fmt, ...)
    __attribute__((format(printf, 4, 5)));

IrVal irwriter_alloca(IrWriter* w, const char* ty);
IrVal irwriter_load(IrWriter* w, const char* ty, IrVal ptr);
void irwriter_store(IrWriter* w, const char* ty, IrVal val, IrVal ptr);
IrVal irwriter_next_reg(IrWriter* w);
void irwriter_emit_val(IrWriter* w, IrVal val);

IrVal irwriter_phi2(IrWriter* w, const char* ty, IrVal v1, IrLabel bb1, IrVal v2, IrLabel bb2);
IrVal irwriter_sext(IrWriter* w, const char* from_ty, IrVal val, const char* to_ty);

void irwriter_type_def(IrWriter* w, const char* name, const char* body);

void irwriter_raw(IrWriter* w, const char* text);
void irwriter_rawf(IrWriter* w, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void irwriter_vrawf(IrWriter* w, const char* fmt, va_list ap);

void irwriter_comment(IrWriter* w, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
