#include "irwriter.h"
#include "darray.h"
#include "symtab.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int id;
  int32_t line;
  int32_t col;
  int scope_id;
} PendingLoc;

struct IrWriter {
  FILE* out;
  char* imms;
  char* labels;
  int reg;
  int label;
  int32_t dbg_line;
  int32_t dbg_col;
  int dbg_next_id;
  int dbg_sub_id;
  int in_switch;
  char* entry_prologue;
  PendingLoc* locs;
  int last_dbg_id;
  int32_t last_dbg_line;
  int32_t last_dbg_col;
  int last_dbg_scope_id;
  int dbg_file_id;
  int dbg_flags_emitted;
  int switch_dbg_id;
  const char* target_triple;
  const char* source_file;
  const char* directory;
  Symtab decls;
};

static void _validate_name(const char* s, const char* label) {
  for (const char* p = s; *p; p++) {
    if (*p == '"' || *p == '\\') {
      fprintf(stderr, "irwriter: %s contains invalid character '%c'\n", label, *p);
      abort();
    }
  }
}

static void _validate_triple(const char* s) {
  if (!s) {
    return;
  }
  if (!*s) {
    fprintf(stderr, "irwriter: target_triple is empty\n");
    abort();
  }
  for (const char* p = s; *p; p++) {
    char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
        c == '-') {
      continue;
    }
    fprintf(stderr, "irwriter: target_triple contains invalid character '%c'\n", c);
    abort();
  }
}

IrWriter* irwriter_new(FILE* out, const char* target_triple) {
  if (target_triple) {
    _validate_triple(target_triple);
  }
  IrWriter* w = calloc(1, sizeof(IrWriter));
  w->out = out;
  w->target_triple = target_triple;
  symtab_init(&w->decls, 0);
  w->dbg_line = -1;
  w->last_dbg_id = -1;
  w->last_dbg_line = -1;
  w->last_dbg_col = -1;
  w->last_dbg_scope_id = -1;
  w->imms = darray_new(1, 1);
  w->imms[0] = '\0';
  w->labels = darray_new(1, 1);
  w->labels[0] = '\0';
  return w;
}

void irwriter_del(IrWriter* w) {
  symtab_free(&w->decls);
  darray_del(w->locs);
  darray_del(w->imms);
  darray_del(w->labels);
  free(w->entry_prologue);
  free(w);
}

void irwriter_start(IrWriter* w, const char* source_file, const char* directory) {
  _validate_name(source_file, "source_file");
  _validate_name(directory, "directory");
  w->source_file = source_file;
  w->directory = directory;
  fprintf(w->out, "source_filename = \"%s\"\n", source_file);
  if (w->target_triple) {
    fprintf(w->out, "target triple = \"%s\"\n", w->target_triple);
  }
  fprintf(w->out, "\n");
}

void irwriter_end(IrWriter* w) {
  if (!w->dbg_flags_emitted) {
    return;
  }
}

static IrVal _next_reg(IrWriter* w) { return (IrVal)w->reg++; }

static void _emit_val(FILE* out, const char* imms, IrVal v) {
  if (v < 0) {
    fprintf(out, "%s", imms + (-v));
  } else {
    fprintf(out, "%%r%d", (int)v);
  }
}

static void _emit_label(FILE* out, const char* labels, IrLabel l) {
  if (l < 0) {
    fprintf(out, "%s", labels + (-l));
  } else {
    fprintf(out, "L%d", (int)l);
  }
}

static void _push_loc(IrWriter* w, int id, int32_t line, int32_t col, int scope_id) {
  if (!w->locs) {
    w->locs = darray_new(sizeof(PendingLoc), 0);
  }
  darray_push(w->locs, ((PendingLoc){id, line, col, scope_id}));
}

static int _reserve_dbg(IrWriter* w) {
  if (w->dbg_line < 0) {
    return -1;
  }
  if (w->last_dbg_id >= 0 && w->last_dbg_line == w->dbg_line && w->last_dbg_col == w->dbg_col &&
      w->last_dbg_scope_id == w->dbg_sub_id) {
    return w->last_dbg_id;
  }
  int id = w->dbg_next_id++;
  _push_loc(w, id, w->dbg_line, w->dbg_col, w->dbg_sub_id);
  w->last_dbg_id = id;
  w->last_dbg_line = w->dbg_line;
  w->last_dbg_col = w->dbg_col;
  w->last_dbg_scope_id = w->dbg_sub_id;
  return id;
}

static void _emit_dbg_suffix(IrWriter* w, int id) {
  if (id >= 0) {
    fprintf(w->out, ", !dbg !%d", id);
  }
}

void irwriter_define_startf(IrWriter* w, const char* name, const char* sig_fmt, ...) {
  _validate_name(name, "function_name");
  symtab_intern(&w->decls, name);
  if (!w->dbg_flags_emitted) {
    w->dbg_flags_emitted = 1;
    w->dbg_next_id = 5;
    w->dbg_file_id = 2;

    fprintf(w->out, "!llvm.module.flags = !{!0, !1}\n");
    fprintf(w->out, "!llvm.dbg.cu = !{!3}\n\n");
    fprintf(w->out, "!0 = !{i32 7, !\"Dwarf Version\", i32 5}\n");
    fprintf(w->out, "!1 = !{i32 2, !\"Debug Info Version\", i32 3}\n");
    fprintf(w->out, "!2 = !DIFile(filename: \"%s\", directory: \"%s\")\n", w->source_file, w->directory);
    fprintf(w->out, "!3 = distinct !DICompileUnit(language: DW_LANG_C11, file: !2,"
                    " producer: \"dfa_gen\", isOptimized: true, runtimeVersion: 0,"
                    " emissionKind: FullDebug)\n");
    fprintf(w->out, "!4 = !DISubroutineType(types: !{null})\n\n");
  }

  w->dbg_sub_id = w->dbg_next_id++;
  fprintf(w->out,
          "!%d = distinct !DISubprogram(name: \"%s\", scope: !%d, file: !%d,"
          " line: 1, type: !4, scopeLine: 1,"
          " spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !3)\n\n",
          w->dbg_sub_id, name, w->dbg_file_id, w->dbg_file_id);

  w->reg = 0;
  w->label = 0;
  w->last_dbg_id = -1;
  w->last_dbg_line = -1;
  w->last_dbg_col = -1;
  w->last_dbg_scope_id = -1;
  w->labels = darray_grow(w->labels, 1);
  free(w->entry_prologue);
  w->entry_prologue = NULL;

  fprintf(w->out, "define ");
  va_list ap;
  va_start(ap, sig_fmt);
  vfprintf(w->out, sig_fmt, ap);
  va_end(ap);
  fprintf(w->out, " !dbg !%d {\n", w->dbg_sub_id);
}

void irwriter_define_end(IrWriter* w) {
  fprintf(w->out, "}\n\n");
  for (size_t i = 0; i < darray_size(w->locs); i++) {
    PendingLoc* l = &w->locs[i];
    fprintf(w->out, "!%d = !DILocation(line: %d, column: %d, scope: !%d)\n", l->id, l->line, l->col, l->scope_id);
  }
  if ((int)darray_size(w->locs) > 0) {
    fprintf(w->out, "\n");
  }
  if (w->locs) {
    w->locs = darray_grow(w->locs, 0);
  }
}

IrLabel irwriter_label(IrWriter* w) { return w->label++; }

IrLabel irwriter_label_f(IrWriter* w, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);

  intptr_t offset = (intptr_t)darray_size(w->labels);
  w->labels = darray_grow(w->labels, darray_size(w->labels) + (size_t)len + 1);

  va_start(ap, fmt);
  vsnprintf(w->labels + offset, (size_t)len + 1, fmt, ap);
  va_end(ap);

  return -(int32_t)offset;
}

IrLabel irwriter_bb(IrWriter* w) {
  IrLabel id = w->label++;
  fprintf(w->out, "L%d:\n", id);
  if (w->entry_prologue) {
    fputs(w->entry_prologue, w->out);
    free(w->entry_prologue);
    w->entry_prologue = NULL;
  }
  return id;
}

void irwriter_bb_at(IrWriter* w, IrLabel label) {
  _emit_label(w->out, w->labels, label);
  fprintf(w->out, ":\n");
}

void irwriter_dbg(IrWriter* w, int32_t line, int32_t col) {
  w->dbg_line = line;
  w->dbg_col = col;
}

IrVal irwriter_imm(IrWriter* w, const char* literal) {
  intptr_t offset = (intptr_t)darray_size(w->imms);
  size_t len = strlen(literal) + 1;
  w->imms = darray_grow(w->imms, darray_size(w->imms) + len);
  memcpy(w->imms + offset, literal, len);
  return -offset;
}

IrVal irwriter_imm_int(IrWriter* w, int v) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", v);
  return irwriter_imm(w, buf);
}

IrVal irwriter_binop(IrWriter* w, const char* op, const char* ty, IrVal lhs, IrVal rhs) {
  IrVal r = _next_reg(w);
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  %%r%d = %s %s ", (int)r, op, ty);
  _emit_val(w->out, w->imms, lhs);
  fprintf(w->out, ", ");
  _emit_val(w->out, w->imms, rhs);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

IrVal irwriter_icmp(IrWriter* w, const char* pred, const char* ty, IrVal lhs, IrVal rhs) {
  IrVal r = _next_reg(w);
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  %%r%d = icmp %s %s ", (int)r, pred, ty);
  _emit_val(w->out, w->imms, lhs);
  fprintf(w->out, ", ");
  _emit_val(w->out, w->imms, rhs);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

void irwriter_br(IrWriter* w, IrLabel label) {
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  br label %%");
  _emit_label(w->out, w->labels, label);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
}

void irwriter_br_cond(IrWriter* w, IrVal cond, IrLabel if_true, IrLabel if_false) {
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  br i1 ");
  _emit_val(w->out, w->imms, cond);
  fprintf(w->out, ", label %%");
  _emit_label(w->out, w->labels, if_true);
  fprintf(w->out, ", label %%");
  _emit_label(w->out, w->labels, if_false);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
}

void irwriter_switch_start(IrWriter* w, const char* ty, IrVal val, IrLabel default_label) {
  w->switch_dbg_id = _reserve_dbg(w);
  fprintf(w->out, "  switch %s ", ty);
  _emit_val(w->out, w->imms, val);
  fprintf(w->out, ", label %%");
  _emit_label(w->out, w->labels, default_label);
  fprintf(w->out, " [\n");
  w->in_switch = 1;
}

void irwriter_switch_case(IrWriter* w, const char* ty, int64_t val, IrLabel label) {
  fprintf(w->out, "    %s %lld, label %%", ty, (long long)val);
  _emit_label(w->out, w->labels, label);
  fprintf(w->out, "\n");
}

void irwriter_switch_end(IrWriter* w) {
  fprintf(w->out, "  ]");
  _emit_dbg_suffix(w, w->switch_dbg_id);
  fprintf(w->out, "\n");
  w->in_switch = 0;
}

void irwriter_ret(IrWriter* w, const char* ty, IrVal val) {
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  ret %s ", ty);
  _emit_val(w->out, w->imms, val);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
}

void irwriter_ret_void(IrWriter* w) {
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  ret void");
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
}

IrVal irwriter_insertvalue(IrWriter* w, const char* agg_ty, IrVal agg, const char* elem_ty, IrVal elem, int idx) {
  IrVal r = _next_reg(w);
  int dbg = _reserve_dbg(w);
  if (agg < 0) {
    fprintf(w->out, "  %%r%d = insertvalue %s undef, %s ", (int)r, agg_ty, elem_ty);
    _emit_val(w->out, w->imms, elem);
    fprintf(w->out, ", %d", idx);
  } else {
    fprintf(w->out, "  %%r%d = insertvalue %s %%r%d, %s ", (int)r, agg_ty, (int)agg, elem_ty);
    _emit_val(w->out, w->imms, elem);
    fprintf(w->out, ", %d", idx);
  }
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

IrVal irwriter_extractvalue(IrWriter* w, const char* agg_ty, IrVal agg, int idx) {
  IrVal r = _next_reg(w);
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  %%r%d = extractvalue %s ", (int)r, agg_ty);
  _emit_val(w->out, w->imms, agg);
  fprintf(w->out, ", %d", idx);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

void irwriter_declare(IrWriter* w, const char* ret_type, const char* name, const char* arg_types) {
  if (symtab_find(&w->decls, name) >= 0) {
    return;
  }
  symtab_intern(&w->decls, name);
  fprintf(w->out, "declare %s @%s(%s)\n\n", ret_type, name, arg_types);
}

void irwriter_call_void_fmtf(IrWriter* w, const char* name, const char* args_fmt, ...) {
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  call void @%s(", name);
  va_list ap;
  va_start(ap, args_fmt);
  vfprintf(w->out, args_fmt, ap);
  va_end(ap);
  fprintf(w->out, ")");
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
}

IrVal irwriter_call_retf(IrWriter* w, const char* ret_ty, const char* name, const char* args_fmt, ...) {
  IrVal r = _next_reg(w);
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  %%r%d = call %s @%s(", (int)r, ret_ty, name);
  va_list ap;
  va_start(ap, args_fmt);
  vfprintf(w->out, args_fmt, ap);
  va_end(ap);
  fprintf(w->out, ")");
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

IrVal irwriter_alloca(IrWriter* w, const char* ty) {
  IrVal r = _next_reg(w);
  fprintf(w->out, "  %%r%d = alloca %s\n", (int)r, ty);
  return r;
}

IrVal irwriter_load(IrWriter* w, const char* ty, IrVal ptr) {
  IrVal r = _next_reg(w);
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  %%r%d = load %s, ptr ", (int)r, ty);
  _emit_val(w->out, w->imms, ptr);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

void irwriter_store(IrWriter* w, const char* ty, IrVal val, IrVal ptr) {
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  store %s ", ty);
  _emit_val(w->out, w->imms, val);
  fprintf(w->out, ", ptr ");
  _emit_val(w->out, w->imms, ptr);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
}

IrVal irwriter_next_reg(IrWriter* w) { return _next_reg(w); }

void irwriter_emit_val(IrWriter* w, IrVal val) { _emit_val(w->out, w->imms, val); }

void irwriter_emit_label(IrWriter* w, IrLabel label) { _emit_label(w->out, w->labels, label); }

IrVal irwriter_sext(IrWriter* w, const char* from_ty, IrVal val, const char* to_ty) {
  IrVal r = _next_reg(w);
  fprintf(w->out, "  %%r%d = sext %s ", (int)r, from_ty);
  _emit_val(w->out, w->imms, val);
  fprintf(w->out, " to %s\n", to_ty);
  return r;
}

void irwriter_type_def(IrWriter* w, const char* name, const char* body) {
  fprintf(w->out, "%%%s = type %s\n", name, body);
}

void irwriter_raw(IrWriter* w, const char* text) { fprintf(w->out, "%s", text); }

void irwriter_rawf(IrWriter* w, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(w->out, fmt, ap);
  va_end(ap);
}

void irwriter_vrawf(IrWriter* w, const char* fmt, va_list ap) { vfprintf(w->out, fmt, ap); }

void irwriter_comment(IrWriter* w, const char* fmt, ...) {
  fprintf(w->out, "  ; ");
  va_list ap;
  va_start(ap, fmt);
  vfprintf(w->out, fmt, ap);
  va_end(ap);
  fprintf(w->out, "\n");
}
