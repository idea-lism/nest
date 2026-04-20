#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

typedef struct IrWriter IrWriter;
typedef int32_t IrVal;
typedef int32_t IrLabel;
typedef IrLabel* IrLabels; // darray

IrWriter* irwriter_new(FILE* out);
void irwriter_del(IrWriter* w);

void irwriter_start(IrWriter* w, int32_t starting_debug_info_number, const char* source_file, const char* directory);
void irwriter_end(IrWriter* w);

// mark a function name as already defined (e.g. by rt prelude), so irwriter_declare skips it
void irwriter_pre_define(IrWriter* w, const char* name);

// set debug base IDs (file, compile unit, subroutine type) — called by irwriter_gen_rt after emitting debug base
void irwriter_set_dbg_base(IrWriter* w, int32_t file_id, int32_t cu_id, int32_t type_id);

// compile nest runtime to LLVM IR and write to IrWriter's stream
void irwriter_gen_rt(IrWriter* w, const char* source_file, const char* directory);
// simple version: pipes empty string to cc for module prelude only
void irwriter_gen_rt_simple(IrWriter* w);

// sig_fmt: everything after "define" and before "{"
// e.g. "internal void @save(ptr %%stack_ptr, ptr %%col)"
// name: function name for debug metadata (DISubprogram)
void irwriter_define_startf(IrWriter* w, const char* name, const char* sig_fmt, ...);
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
void irwriter_emit_label(IrWriter* w, IrLabel label);

IrVal irwriter_sext(IrWriter* w, const char* from_ty, IrVal val, const char* to_ty);

void irwriter_type_def(IrWriter* w, const char* name, const char* body);

void irwriter_raw(IrWriter* w, const char* text);
void irwriter_rawf(IrWriter* w, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void irwriter_vrawf(IrWriter* w, const char* fmt, va_list ap);

FILE* irwriter_get_file(IrWriter* w);

void irwriter_comment(IrWriter* w, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
