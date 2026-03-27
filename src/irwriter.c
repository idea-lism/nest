#include "irwriter.h"
#include "darray.h"
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
  int reg;
  int32_t dbg_line;
  int32_t dbg_col;
  int dbg_next_id;
  int dbg_sub_id;
  int in_switch;
  int widen_ret;
  char* entry_prologue;
  PendingLoc* locs;
  int dbg_file_id;
  int dbg_flags_emitted;
  int switch_dbg_id;
  const char* target_triple;
  const char* source_file;
  const char* directory;
  const char** decls;
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
  _validate_triple(target_triple);
  IrWriter* w = calloc(1, sizeof(IrWriter));
  w->out = out;
  w->target_triple = target_triple;
  w->dbg_line = -1;
  return w;
}

void irwriter_del(IrWriter* w) {
  for (int i = 0; i < (int)darray_size(w->decls); i++) {
    free((void*)w->decls[i]);
  }
  darray_del(w->decls);
  darray_del(w->locs);
  free(w->entry_prologue);
  free(w);
}

void irwriter_start(IrWriter* w, const char* source_file, const char* directory) {
  _validate_name(source_file, "source_file");
  _validate_name(directory, "directory");
  w->source_file = source_file;
  w->directory = directory;
  fprintf(w->out, "source_filename = \"%s\"\n", source_file);
  fprintf(w->out, "target triple = \"%s\"\n\n", w->target_triple);
}

void irwriter_end(IrWriter* w) {
  if (!w->dbg_flags_emitted) {
    return;
  }
}

static int32_t _next_reg(IrWriter* w) { return w->reg++; }

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
  int id = w->dbg_next_id++;
  _push_loc(w, id, w->dbg_line, w->dbg_col, w->dbg_sub_id);
  return id;
}

static void _emit_dbg_suffix(IrWriter* w, int id) {
  if (id >= 0) {
    fprintf(w->out, ", !dbg !%d", id);
  }
}

void irwriter_define_start(IrWriter* w, const char* name, const char* ret_type, int argc, const char** arg_types,
                           const char** arg_names) {
  _validate_name(name, "function_name");
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
  w->widen_ret = strcmp(ret_type, "{i32, i32}") == 0;
  free(w->entry_prologue);
  w->entry_prologue = NULL;

  const char* ext_ret = w->widen_ret ? "{i64, i64}" : ret_type;
  fprintf(w->out, "define %s @%s(", ext_ret, name);
  for (int i = 0; i < argc; i++) {
    if (i) {
      fprintf(w->out, ", ");
    }
    const char* ext_ty = strcmp(arg_types[i], "i32") == 0 ? "i64" : arg_types[i];
    const char* suffix = strcmp(arg_types[i], "i32") == 0 ? "_i64" : "";
    fprintf(w->out, "%s %%%s%s", ext_ty, arg_names[i], suffix);
  }
  fprintf(w->out, ") !dbg !%d {\n", w->dbg_sub_id);

  char buf[1024] = {0};
  int pos = 0;
  for (int i = 0; i < argc; i++) {
    if (strcmp(arg_types[i], "i32") == 0) {
      pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "  %%%s = trunc i64 %%%s_i64 to i32\n", arg_names[i],
                      arg_names[i]);
    }
  }
  if (pos > 0) {
    w->entry_prologue = strdup(buf);
  }
}

void irwriter_define_end(IrWriter* w) {
  fprintf(w->out, "}\n\n");
  for (int i = 0; i < (int)darray_size(w->locs); i++) {
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

void irwriter_bb(IrWriter* w, const char* label) {
  fprintf(w->out, "%s:\n", label);
  if (w->entry_prologue) {
    fputs(w->entry_prologue, w->out);
    free(w->entry_prologue);
    w->entry_prologue = NULL;
  }
}

void irwriter_dbg(IrWriter* w, int32_t line, int32_t col) {
  w->dbg_line = line;
  w->dbg_col = col;
}

int32_t irwriter_binop(IrWriter* w, const char* op, const char* ty, const char* lhs, const char* rhs) {
  int32_t r = _next_reg(w);
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  %%r%d = %s %s %s, %s", r, op, ty, lhs, rhs);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

int32_t irwriter_binop_imm(IrWriter* w, const char* op, const char* ty, const char* lhs, int64_t rhs) {
  int32_t r = _next_reg(w);
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  %%r%d = %s %s %s, %lld", r, op, ty, lhs, (long long)rhs);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

int32_t irwriter_icmp(IrWriter* w, const char* pred, const char* ty, const char* lhs, const char* rhs) {
  int32_t r = _next_reg(w);
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  %%r%d = icmp %s %s %s, %s", r, pred, ty, lhs, rhs);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

int32_t irwriter_icmp_imm(IrWriter* w, const char* pred, const char* ty, const char* lhs, int64_t rhs) {
  int32_t r = _next_reg(w);
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  %%r%d = icmp %s %s %s, %lld", r, pred, ty, lhs, (long long)rhs);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

void irwriter_br(IrWriter* w, const char* label) {
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  br label %%%s", label);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
}

void irwriter_br_cond(IrWriter* w, const char* cond, const char* if_true, const char* if_false) {
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  br i1 %s, label %%%s, label %%%s", cond, if_true, if_false);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
}

void irwriter_switch_start(IrWriter* w, const char* ty, const char* val, const char* default_label) {
  w->switch_dbg_id = _reserve_dbg(w);
  fprintf(w->out, "  switch %s %s, label %%%s [\n", ty, val, default_label);
  w->in_switch = 1;
}

void irwriter_switch_case(IrWriter* w, const char* ty, int64_t val, const char* label) {
  fprintf(w->out, "    %s %lld, label %%%s\n", ty, (long long)val, label);
}

void irwriter_switch_end(IrWriter* w) {
  fprintf(w->out, "  ]");
  _emit_dbg_suffix(w, w->switch_dbg_id);
  fprintf(w->out, "\n");
  w->in_switch = 0;
}

void irwriter_ret(IrWriter* w, const char* ty, const char* val) {
  int dbg = _reserve_dbg(w);
  if (w->widen_ret && strcmp(ty, "{i32, i32}") == 0) {
    int32_t e0 = _next_reg(w);
    fprintf(w->out, "  %%r%d = extractvalue {i32, i32} %s, 0\n", e0, val);
    int32_t e1 = _next_reg(w);
    fprintf(w->out, "  %%r%d = extractvalue {i32, i32} %s, 1\n", e1, val);
    int32_t s0 = _next_reg(w);
    fprintf(w->out, "  %%r%d = sext i32 %%r%d to i64\n", s0, e0);
    int32_t s1 = _next_reg(w);
    fprintf(w->out, "  %%r%d = sext i32 %%r%d to i64\n", s1, e1);
    int32_t w0 = _next_reg(w);
    fprintf(w->out, "  %%r%d = insertvalue {i64, i64} undef, i64 %%r%d, 0\n", w0, s0);
    int32_t w1 = _next_reg(w);
    fprintf(w->out, "  %%r%d = insertvalue {i64, i64} %%r%d, i64 %%r%d, 1\n", w1, w0, s1);
    fprintf(w->out, "  ret {i64, i64} %%r%d", w1);
  } else {
    fprintf(w->out, "  ret %s %s", ty, val);
  }
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
}

int32_t irwriter_insertvalue(IrWriter* w, const char* agg_ty, const char* agg_val, const char* elem_ty,
                             const char* elem_val, int idx) {
  int32_t r = _next_reg(w);
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  %%r%d = insertvalue %s %s, %s %s, %d", r, agg_ty, agg_val, elem_ty, elem_val, idx);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

int32_t irwriter_insertvalue_imm(IrWriter* w, const char* agg_ty, const char* agg_val, const char* elem_ty,
                                 int64_t elem_val, int idx) {
  int32_t r = _next_reg(w);
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  %%r%d = insertvalue %s %s, %s %lld, %d", r, agg_ty, agg_val, elem_ty, (long long)elem_val, idx);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

void irwriter_declare(IrWriter* w, const char* ret_type, const char* name, const char* arg_types) {
  for (int i = 0; i < (int)darray_size(w->decls); i++) {
    if (strcmp(w->decls[i], name) == 0) {
      return;
    }
  }
  if (!w->decls) {
    w->decls = darray_new(sizeof(const char*), 0);
  }
  darray_push(w->decls, strdup(name));
  fprintf(w->out, "declare %s @%s(%s)\n\n", ret_type, name, arg_types);
}

void irwriter_call_void(IrWriter* w, const char* name) {
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  call void @%s()", name);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
}

void irwriter_call_void_fmt(IrWriter* w, const char* name, const char* args) {
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  call void @%s(%s)", name, args);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
}

int32_t irwriter_call_ret(IrWriter* w, const char* ret_ty, const char* name, const char* args) {
  int32_t r = _next_reg(w);
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  %%r%d = call %s @%s(%s)", r, ret_ty, name, args);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

int32_t irwriter_alloca(IrWriter* w, const char* ty) {
  int32_t r = _next_reg(w);
  fprintf(w->out, "  %%r%d = alloca %s\n", r, ty);
  return r;
}

int32_t irwriter_load(IrWriter* w, const char* ty, const char* ptr) {
  int32_t r = _next_reg(w);
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  %%r%d = load %s, ptr %s", r, ty, ptr);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

void irwriter_store(IrWriter* w, const char* ty, const char* val, const char* ptr) {
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  store %s %s, ptr %s", ty, val, ptr);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
}

int32_t irwriter_gep(IrWriter* w, const char* base_ty, const char* ptr, const char* indices) {
  int32_t r = _next_reg(w);
  fprintf(w->out, "  %%r%d = getelementptr %s, ptr %s, %s\n", r, base_ty, ptr, indices);
  return r;
}

int32_t irwriter_phi2(IrWriter* w, const char* ty, const char* v1, const char* bb1, const char* v2, const char* bb2) {
  int32_t r = _next_reg(w);
  fprintf(w->out, "  %%r%d = phi %s [ %s, %%%s ], [ %s, %%%s ]\n", r, ty, v1, bb1, v2, bb2);
  return r;
}

int32_t irwriter_select(IrWriter* w, const char* cond, const char* ty, const char* true_val, const char* false_val) {
  int32_t r = _next_reg(w);
  int dbg = _reserve_dbg(w);
  fprintf(w->out, "  %%r%d = select i1 %s, %s %s, %s %s", r, cond, ty, true_val, ty, false_val);
  _emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

int32_t irwriter_sext(IrWriter* w, const char* from_ty, const char* val, const char* to_ty) {
  int32_t r = _next_reg(w);
  fprintf(w->out, "  %%r%d = sext %s %s to %s\n", r, from_ty, val, to_ty);
  return r;
}

void irwriter_type_def(IrWriter* w, const char* name, const char* body) {
  fprintf(w->out, "%%%s = type %s\n", name, body);
}

void irwriter_raw(IrWriter* w, const char* text) { fprintf(w->out, "%s", text); }
