#include "irwriter.h"
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
  int buf_idx;
  char buf[2][32];
  int32_t dbg_line;
  int32_t dbg_col;
  int dbg_next_id;
  int dbg_sub_id;
  int in_switch;
  PendingLoc* locs;
  int nlocs;
  int locs_cap;
  int dbg_file_id;
  int dbg_flags_emitted;
  int switch_dbg_id;
  const char* target_triple;
  const char* source_file;
  const char* directory;
  const char** decls;
  int ndecls;
  int decls_cap;
};

IrWriter* irwriter_new(FILE* out, const char* target_triple) {
  IrWriter* w = calloc(1, sizeof(IrWriter));
  w->out = out;
  w->target_triple = target_triple;
  w->dbg_line = -1;
  return w;
}

void irwriter_del(IrWriter* w) {
  for (int i = 0; i < w->ndecls; i++) {
    free((void*)w->decls[i]);
  }
  free(w->decls);
  free(w->locs);
  free(w);
}

void irwriter_start(IrWriter* w, const char* source_file, const char* directory) {
  w->source_file = source_file;
  w->directory = directory;
  fprintf(w->out, "source_filename = \"%s\"\n", source_file);
  fprintf(w->out, "target triple = \"%s\"\n\n", w->target_triple);
}

void irwriter_end(IrWriter* w) {
  if (!w->dbg_flags_emitted) {
    return;
  }
  // Module flags and dbg.cu already emitted at first define_start
  (void)w;
}

static const char* next_reg(IrWriter* w) {
  int idx = w->buf_idx;
  w->buf_idx = 1 - idx;
  snprintf(w->buf[idx], sizeof(w->buf[idx]), "%%r%d", w->reg++);
  return w->buf[idx];
}

static void push_loc(IrWriter* w, int id, int32_t line, int32_t col, int scope_id) {
  if (w->nlocs == w->locs_cap) {
    w->locs_cap = w->locs_cap ? w->locs_cap * 2 : 64;
    w->locs = realloc(w->locs, (size_t)w->locs_cap * sizeof(PendingLoc));
  }
  w->locs[w->nlocs++] = (PendingLoc){id, line, col, scope_id};
}

// Reserve a metadata id for the current debug location. Returns -1 if none set.
static int reserve_dbg(IrWriter* w) {
  if (w->dbg_line < 0) {
    return -1;
  }
  int id = w->dbg_next_id++;
  push_loc(w, id, w->dbg_line, w->dbg_col, w->dbg_sub_id);
  return id;
}

static void emit_dbg_suffix(IrWriter* w, int id) {
  if (id >= 0) {
    fprintf(w->out, ", !dbg !%d", id);
  }
}

void irwriter_define_start(IrWriter* w, const char* name, const char* ret_type, int argc, const char** arg_types,
                           const char** arg_names) {
  // Emit module-level debug metadata once
  if (!w->dbg_flags_emitted) {
    w->dbg_flags_emitted = 1;
    // Reserve ids 0..4 for module-level debug metadata
    // 0: Dwarf Version flag
    // 1: Debug Info Version flag
    // 2: DIFile
    // 3: DICompileUnit
    // 4: DISubroutineType
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

  // DISubprogram for this function
  w->dbg_sub_id = w->dbg_next_id++;
  // We emit the DISubprogram metadata now (it's ok to interleave with later nodes)
  fprintf(w->out,
          "!%d = distinct !DISubprogram(name: \"%s\", scope: !%d, file: !%d,"
          " line: 1, type: !4, scopeLine: 1,"
          " spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !3)\n\n",
          w->dbg_sub_id, name, w->dbg_file_id, w->dbg_file_id);

  w->reg = 0;

  fprintf(w->out, "define %s @%s(", ret_type, name);
  for (int i = 0; i < argc; i++) {
    if (i) {
      fprintf(w->out, ", ");
    }
    fprintf(w->out, "%s %%%s", arg_types[i], arg_names[i]);
  }
  fprintf(w->out, ") !dbg !%d {\n", w->dbg_sub_id);
}

void irwriter_define_end(IrWriter* w) {
  fprintf(w->out, "}\n\n");
  // Emit pending debug locations
  for (int i = 0; i < w->nlocs; i++) {
    PendingLoc* l = &w->locs[i];
    fprintf(w->out, "!%d = !DILocation(line: %d, column: %d, scope: !%d)\n", l->id, l->line, l->col, l->scope_id);
  }
  if (w->nlocs > 0) {
    fprintf(w->out, "\n");
  }
  w->nlocs = 0;
}

void irwriter_bb(IrWriter* w, const char* label) { fprintf(w->out, "%s:\n", label); }

void irwriter_dbg(IrWriter* w, int32_t line, int32_t col) {
  w->dbg_line = line;
  w->dbg_col = col;
}

const char* irwriter_binop(IrWriter* w, const char* op, const char* ty, const char* lhs, const char* rhs) {
  const char* r = next_reg(w);
  int dbg = reserve_dbg(w);
  fprintf(w->out, "  %s = %s %s %s, %s", r, op, ty, lhs, rhs);
  emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

const char* irwriter_binop_imm(IrWriter* w, const char* op, const char* ty, const char* lhs, int64_t rhs) {
  const char* r = next_reg(w);
  int dbg = reserve_dbg(w);
  fprintf(w->out, "  %s = %s %s %s, %lld", r, op, ty, lhs, (long long)rhs);
  emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

const char* irwriter_icmp(IrWriter* w, const char* pred, const char* ty, const char* lhs, const char* rhs) {
  const char* r = next_reg(w);
  int dbg = reserve_dbg(w);
  fprintf(w->out, "  %s = icmp %s %s %s, %s", r, pred, ty, lhs, rhs);
  emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

const char* irwriter_icmp_imm(IrWriter* w, const char* pred, const char* ty, const char* lhs, int64_t rhs) {
  const char* r = next_reg(w);
  int dbg = reserve_dbg(w);
  fprintf(w->out, "  %s = icmp %s %s %s, %lld", r, pred, ty, lhs, (long long)rhs);
  emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

void irwriter_br(IrWriter* w, const char* label) {
  int dbg = reserve_dbg(w);
  fprintf(w->out, "  br label %%%s", label);
  emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
}

void irwriter_br_cond(IrWriter* w, const char* cond, const char* if_true, const char* if_false) {
  int dbg = reserve_dbg(w);
  fprintf(w->out, "  br i1 %s, label %%%s, label %%%s", cond, if_true, if_false);
  emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
}

void irwriter_switch_start(IrWriter* w, const char* ty, const char* val, const char* default_label) {
  w->switch_dbg_id = reserve_dbg(w);
  fprintf(w->out, "  switch %s %s, label %%%s [\n", ty, val, default_label);
  w->in_switch = 1;
}

void irwriter_switch_case(IrWriter* w, const char* ty, int64_t val, const char* label) {
  fprintf(w->out, "    %s %lld, label %%%s\n", ty, (long long)val, label);
}

void irwriter_switch_end(IrWriter* w) {
  fprintf(w->out, "  ]");
  emit_dbg_suffix(w, w->switch_dbg_id);
  fprintf(w->out, "\n");
  w->in_switch = 0;
}

void irwriter_ret(IrWriter* w, const char* ty, const char* val) {
  int dbg = reserve_dbg(w);
  fprintf(w->out, "  ret %s %s", ty, val);
  emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
}

const char* irwriter_insertvalue(IrWriter* w, const char* agg_ty, const char* agg_val, const char* elem_ty,
                                 const char* elem_val, int idx) {
  const char* r = next_reg(w);
  int dbg = reserve_dbg(w);
  fprintf(w->out, "  %s = insertvalue %s %s, %s %s, %d", r, agg_ty, agg_val, elem_ty, elem_val, idx);
  emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

const char* irwriter_insertvalue_imm(IrWriter* w, const char* agg_ty, const char* agg_val, const char* elem_ty,
                                     int64_t elem_val, int idx) {
  const char* r = next_reg(w);
  int dbg = reserve_dbg(w);
  fprintf(w->out, "  %s = insertvalue %s %s, %s %lld, %d", r, agg_ty, agg_val, elem_ty, (long long)elem_val, idx);
  emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
  return r;
}

void irwriter_declare(IrWriter* w, const char* ret_type, const char* name, const char* arg_types) {
  for (int i = 0; i < w->ndecls; i++) {
    if (strcmp(w->decls[i], name) == 0) {
      return;
    }
  }
  if (w->ndecls == w->decls_cap) {
    w->decls_cap = w->decls_cap ? w->decls_cap * 2 : 8;
    w->decls = realloc(w->decls, (size_t)w->decls_cap * sizeof(const char*));
  }
  w->decls[w->ndecls++] = strdup(name);
  fprintf(w->out, "declare %s @%s(%s)\n\n", ret_type, name, arg_types);
}

void irwriter_call_void(IrWriter* w, const char* name) {
  int dbg = reserve_dbg(w);
  fprintf(w->out, "  call void @%s()", name);
  emit_dbg_suffix(w, dbg);
  fprintf(w->out, "\n");
}
