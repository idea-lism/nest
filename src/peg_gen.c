// specs/peg_gen.md
#include "darray.h"
#include "peg.h"
#include "peg_ir.h"
#include "symtab.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================
// Helpers
// ============================================================

__attribute__((format(printf, 1, 2))) static char* _fmt(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char* buf = malloc((size_t)n + 1);
  va_start(ap, fmt);
  vsnprintf(buf, (size_t)n + 1, fmt, ap);
  va_end(ap);
  return buf;
}

static uint64_t _tag_mask(int32_t bit_size, uint64_t offset) {
  if (bit_size <= 0 || offset >= 64) {
    return 0;
  }
  int32_t available_bits = 64 - (int32_t)offset;
  if (bit_size >= available_bits) {
    return UINT64_MAX << offset;
  }
  return ((1ULL << bit_size) - 1) << offset;
}

static char* _sanitize_field_name(const char* name) {
  size_t len = strlen(name);
  char* out = malloc(len + 2);
  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    char c = name[i];
    out[j++] = (c == '@' || c == '.') ? '_' : c;
  }
  out[j] = '\0';
  return out;
}

// ============================================================
// Header generation
// ============================================================

static void _gen_header_types(HeaderWriter* hw) {
  hw_raw(hw, "#include <stdint.h>\n#include <stdbool.h>\n#include <string.h>\n\n");
  hw_raw(hw, "#ifndef _NEST_TOKEN_TYPES\n#define _NEST_TOKEN_TYPES\n");
  hw_raw(hw, "typedef struct { int32_t term_id; int32_t cp_start; int32_t cp_size; int32_t chunk_id; } Token;\n");
  hw_raw(hw, "typedef Token* Tokens;\n");
  hw_raw(hw, "typedef struct TokenChunk {\n"
             "  int32_t scope_id; int32_t parent_id;\n"
             "  void* value; void* aux_value; Tokens tokens;\n"
             "} TokenChunk;\n");
  hw_raw(hw, "typedef TokenChunk* TokenChunks;\n");
  hw_raw(hw, "typedef struct TokenTree {\n"
             "  const char* src; uint64_t* newline_map;\n"
             "  TokenChunk* root; TokenChunk* current; TokenChunks table;\n"
             "} TokenTree;\n");
  hw_raw(hw, "#endif\n\n");
  hw_raw(hw, "#ifndef _NEST_PEGREF\n#define _NEST_PEGREF\n");
  hw_raw(hw, "typedef struct { TokenChunk* tc; int64_t col; int64_t row; } PegRef;\n");
  hw_raw(hw, "#endif\n\n");
  hw_raw(hw, "typedef struct {\n  int64_t rhs_row;\n  int64_t col_size_in_i32;\n  PegRef elem;\n} PegLink;\n");
  hw_blank(hw);
}

static void _gen_node_struct(HeaderWriter* hw, ScopedRule* sr, const char* rule_name) {
  if (!sr->node_fields) {
    return;
  }
  int32_t tag_size = symtab_count(&sr->tags);
  hw_fmt(hw, "typedef struct {\n");
  if (tag_size > 0) {
    hw_raw(hw, "  struct {\n");
    for (int32_t t = 0; t < tag_size; t++) {
      char* san = _sanitize_field_name(symtab_get(&sr->tags, t + sr->tags.start_num));
      hw_fmt(hw, "    bool %s : 1;\n", san);
      free(san);
    }
    if (tag_size < 64) {
      hw_fmt(hw, "    uint64_t _padding : %d;\n", 64 - tag_size);
    }
    hw_raw(hw, "  } is;\n");
  }
  for (size_t i = 0; i < darray_size(sr->node_fields); i++) {
    NodeField* nf = &sr->node_fields[i];
    hw_fmt(hw, "  %s %s;\n", nf->is_link ? "PegLink" : "PegRef", nf->name);
  }
  hw_fmt(hw, "} Node_%s;\n", rule_name);
  hw_blank(hw);
}

// Info for one scope's instance of a rule, used in loader generation.
typedef struct {
  int32_t scope_id;
  ScopedRule* sr;
  ScopeClosure* cl;
} RuleScopeEntry;

typedef struct {
  RuleScopeEntry** entries;
  int32_t* counts;
  int32_t start_num;
  int32_t size;
} RuleScopeMap;

static RuleScopeMap _build_scope_map(PegGenInput* input) {
  ScopeClosure* closures = input->scope_closures;
  int32_t closure_size = (int32_t)darray_size(closures);
  RuleScopeMap map = {
      .start_num = input->rule_names.start_num,
      .size = symtab_count(&input->rule_names),
  };
  map.entries = calloc((size_t)map.size, sizeof(RuleScopeEntry*));
  map.counts = calloc((size_t)map.size, sizeof(int32_t));
  for (int32_t c = 0; c < closure_size; c++) {
    ScopeClosure* cl = &closures[c];
    for (size_t i = 0; i < darray_size(cl->scoped_rules); i++) {
      ScopedRule* sr = &cl->scoped_rules[i];
      if (sr->original_global_id < 0) {
        continue;
      }
      int32_t idx = sr->original_global_id - map.start_num;
      if (idx < 0 || idx >= map.size) {
        continue;
      }
      int32_t n = map.counts[idx];
      map.entries[idx] = realloc(map.entries[idx], (size_t)(n + 1) * sizeof(RuleScopeEntry));
      map.entries[idx][n] = (RuleScopeEntry){.scope_id = cl->scope_id, .sr = sr, .cl = cl};
      map.counts[idx] = n + 1;
    }
  }
  return map;
}

static void _free_scope_map(RuleScopeMap* map) {
  for (int32_t i = 0; i < map->size; i++) {
    free(map->entries[i]);
  }
  free(map->entries);
  free(map->counts);
}

static void _gen_loader(HeaderWriter* hw, const char* rule_name, RuleScopeEntry* scope_entries,
                        int32_t used_in_closures_count, const char* prefix) {
  if (used_in_closures_count == 0) {
    return;
  }
  ScopedRule* sr0 = scope_entries[0].sr;
  if (!sr0->node_fields) {
    return;
  }
  int32_t tag_size = symtab_count(&sr0->tags);
  bool has_tags = (tag_size > 0);

  hw_fmt(hw, "static inline Node_%s %s_load_%s(PegRef ref) {\n", rule_name, prefix, rule_name);
  hw_fmt(hw, "  Node_%s n;\n", rule_name);
  hw_raw(hw, "  memset(&n, 0, sizeof(n));\n");
  hw_raw(hw, "  int64_t* table = (int64_t*)ref.tc->value;\n");
  hw_raw(hw, "  int64_t _cur = ref.col;\n");

  hw_raw(hw, "  switch (ref.tc->scope_id) {\n");
  for (int32_t e = 0; e < used_in_closures_count; e++) {
    ScopedRule* sr = scope_entries[e].sr;
    ScopeClosure* cl = scope_entries[e].cl;
    int64_t col_size_in_i32 = cl->bits_bucket_size * 2 + cl->slots_size;

    hw_fmt(hw, "  case %d: {\n", scope_entries[e].scope_id);

    if (has_tags) {
      hw_fmt(hw, "    int64_t* col = (int64_t*)((int32_t*)table + %lld * ref.col);\n", (long long)col_size_in_i32);
      hw_fmt(hw, "    ((uint64_t*)&n.is)[0] = (col[%llu] >> %lluULL) & 0x%llxULL;\n",
             (unsigned long long)sr->tag_bit_index, (unsigned long long)sr->tag_bit_offset,
             (unsigned long long)_tag_mask(tag_size, 0));
    }
    for (size_t fi = 0; fi < darray_size(sr->node_fields); fi++) {
      NodeField* nf = &sr->node_fields[fi];
      if (nf->is_link) {
        if (nf->is_scope) {
          hw_fmt(hw, "    n.%s.elem.tc = &((TokenTree*)ref.tc->aux_value)->table[ref.tc->tokens[_cur].chunk_id];\n",
                 nf->name);
          hw_fmt(hw, "    n.%s.elem.col = 0;\n", nf->name);
        } else {
          hw_fmt(hw, "    n.%s.elem.tc = ref.tc;\n    n.%s.elem.col = _cur;\n", nf->name, nf->name);
        }
        hw_fmt(hw, "    n.%s.elem.row = %d;\n", nf->name, nf->ref_row);
        hw_fmt(hw, "    n.%s.col_size_in_i32 = %lld;\n", nf->name, (long long)col_size_in_i32);
        hw_fmt(hw, "    n.%s.rhs_row = %d;\n", nf->name, nf->rhs_row);
      } else {
        if (nf->is_scope) {
          hw_fmt(hw, "    n.%s.tc = &((TokenTree*)ref.tc->aux_value)->table[ref.tc->tokens[_cur].chunk_id];\n",
                 nf->name);
          hw_fmt(hw, "    n.%s.col = 0;\n    n.%s.row = 0;\n", nf->name, nf->name);
        } else {
          hw_fmt(hw, "    n.%s.tc = ref.tc;\n    n.%s.col = _cur;\n    n.%s.row = %d;\n", nf->name, nf->name, nf->name,
                 nf->ref_row);
        }
      }

      switch (nf->advance) {
      case NODE_ADVANCE_ONE:
        hw_raw(hw, "    _cur++;\n");
        break;
      case NODE_ADVANCE_SLOT:
        hw_fmt(hw, "    _cur += ((int32_t*)ref.tc->value)[_cur * %lld + %d];\n", (long long)col_size_in_i32,
               nf->advance_slot_row);
        break;
      case NODE_ADVANCE_NONE:
        break;
      }
    }

    hw_raw(hw, "    break;\n  }\n");
  }
  hw_raw(hw, "  }\n");
  hw_raw(hw, "  return n;\n}\n");
  hw_blank(hw);
}

static void _gen_has_next(HeaderWriter* hw, const char* prefix) {
  hw_fmt(hw, "static inline bool %s_has_next(PegLink l) {\n", prefix);
  hw_raw(hw, "  int32_t* col = (int32_t*)l.elem.tc->value + l.col_size_in_i32 * l.elem.col;\n");
  hw_raw(hw, "  int32_t lhs_slot = col[l.elem.row];\n");
  hw_raw(hw, "  if (lhs_slot < 0) return false;\n");
  hw_raw(hw, "  if (l.rhs_row >= 0) {\n");
  hw_raw(hw, "    int32_t* rhs_col = col + l.col_size_in_i32 * lhs_slot;\n");
  hw_raw(hw, "    int32_t rhs_slot = rhs_col[l.rhs_row];\n");
  hw_raw(hw, "    if (rhs_slot < 0) return false;\n");
  hw_raw(hw, "  }\n");
  hw_raw(hw, "  return true;\n}\n");
  hw_blank(hw);
}

static void _gen_get_next(HeaderWriter* hw, const char* prefix) {
  hw_fmt(hw, "static inline PegLink %s_get_next(PegLink l) {\n", prefix);
  hw_raw(hw, "  int32_t* col = (int32_t*)l.elem.tc->value + l.col_size_in_i32 * l.elem.col;\n");
  hw_raw(hw, "  int32_t lhs_slot = col[l.elem.row];\n");
  hw_raw(hw, "  if (l.rhs_row >= 0) {\n");
  hw_raw(hw, "    int32_t* rhs_col = col + l.col_size_in_i32 * lhs_slot;\n");
  hw_raw(hw, "    int32_t rhs_slot = rhs_col[l.rhs_row];\n");
  hw_raw(hw, "    l.elem.col += lhs_slot + rhs_slot;\n");
  hw_raw(hw, "  } else {\n");
  hw_raw(hw, "    l.elem.col += lhs_slot;\n");
  hw_raw(hw, "  }\n");
  hw_raw(hw, "  return l;\n}\n");
  hw_blank(hw);
}

static void _gen_peg_size(HeaderWriter* hw, ScopeClosure* closures, int32_t closure_size, const char* prefix) {
  hw_fmt(hw, "static inline int64_t %s_peg_size(PegRef ref) {\n", prefix);
  hw_raw(hw, "  if (!ref.tc || !ref.tc->value) return -1;\n");
  hw_raw(hw, "  switch (ref.tc->scope_id) {\n");
  for (int32_t c = 0; c < closure_size; c++) {
    ScopeClosure* cl = &closures[c];
    int64_t col_size_in_i32 = cl->bits_bucket_size * 2 + cl->slots_size;
    // ref.row is an absolute i32 row within { i64 bits[]; i32 slots[] }.
    hw_fmt(hw, "  case %d: return ((int32_t*)ref.tc->value)[ref.col * %lld + ref.row];\n", cl->scope_id,
           (long long)col_size_in_i32);
  }
  hw_raw(hw, "  default: return -1;\n");
  hw_raw(hw, "  }\n}\n");
  hw_blank(hw);
}

static void _gen_header(PegGenInput* input, HeaderWriter* hw, RuleScopeMap* scope_map) {
  ScopeClosure* closures = input->scope_closures;
  int32_t closure_size = (int32_t)darray_size(closures);
  const char* prefix = input->prefix;

  _gen_header_types(hw);

  // emit node structs and loaders for each distinct global rule
  for (int32_t gid = 0; gid < scope_map->size; gid++) {
    if (scope_map->counts[gid] == 0) {
      continue;
    }
    const char* rule_name = symtab_get(&input->rule_names, gid + scope_map->start_num);
    _gen_node_struct(hw, scope_map->entries[gid][0].sr, rule_name);
    _gen_loader(hw, rule_name, scope_map->entries[gid], scope_map->counts[gid], prefix);
  }

  _gen_has_next(hw, prefix);
  _gen_get_next(hw, prefix);
  _gen_peg_size(hw, closures, closure_size, prefix);
}

// ============================================================
// LLVM IR generation
// ============================================================

static void _gen_scope_ir(IrWriter* w, ScopeClosure* cl, int memoize_mode) {
  int32_t rule_size = (int32_t)darray_size(cl->scoped_rules);
  if (rule_size == 0) {
    return;
  }
  int64_t sizeof_col = cl->bits_bucket_size * 8 + cl->slots_size * 4;

  irwriter_declare(w, "ptr", "tt_current", "ptr");
  irwriter_declare(w, "ptr", "tt_alloc_memoize_table", "ptr, i64");
  irwriter_declare(w, "i32", "tt_current_size", "ptr");

  char* fn_name = _fmt("parse_%s", cl->scope_name);
  irwriter_define_startf(w, fn_name, "{i64, i64} @%s(ptr %%tt, ptr %%stack_ptr_in)", fn_name);
  irwriter_bb(w);
  irwriter_dbg(w, cl->source_line, cl->source_col);

  IrVal tc = irwriter_call_retf(w, "ptr", "tt_current", "ptr %%tt");
  IrVal peg_table =
      irwriter_call_retf(w, "ptr", "tt_alloc_memoize_table", "ptr %%r%d, i64 %lld", (int)tc, (long long)sizeof_col);

  IrVal col_ptr = irwriter_alloca(w, "i64");
  irwriter_store(w, "i64", irwriter_imm_int(w, 0), col_ptr);
  IrVal stack_ptr = irwriter_alloca(w, "ptr");
  irwriter_store(w, "ptr", irwriter_imm(w, "%stack_ptr_in"), stack_ptr);
  IrVal parse_result = irwriter_alloca(w, "i64");
  irwriter_store(w, "i64", irwriter_imm_int(w, -1), parse_result);
  IrVal parsed_tokens = irwriter_alloca(w, "i64");
  irwriter_store(w, "i64", irwriter_imm_int(w, 0), parsed_tokens);
  IrVal tag_bits = irwriter_alloca(w, "i64");
  irwriter_store(w, "i64", irwriter_imm_int(w, 0), tag_bits);

  IrVal token_size = irwriter_call_retf(w, "i32", "tt_current_size", "ptr %%tt");

  irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%r%d, i64 24\n", irwriter_next_reg(w), (int)tc);
  IrVal tokens = irwriter_load(w, "ptr", (IrVal)(irwriter_next_reg(w) - 1));

  PegIrCtx ctx = {
      .ir_writer = w,
      .fn_name = fn_name,
      .tc = tc,
      .tokens = tokens,
      .col = col_ptr,
      .token_size = token_size,
      .stack_ptr = stack_ptr,
      .parse_result = parse_result,
      .tag_bits = tag_bits,
      .parsed_tokens = parsed_tokens,
      .ret_labels = darray_new(sizeof(IrLabel), 0),
  };

  IrLabel final_ret = peg_ir_emit_call(&ctx, cl->scoped_rules[0].scoped_rule_name);

  for (int32_t i = 0; i < rule_size; i++) {
    ScopedRule* sr = &cl->scoped_rules[i];
    irwriter_rawf(w, "\n%s:\n", sr->scoped_rule_name);

    int32_t tag_size = symtab_count(&sr->tags);
    ctx.tag_bit_offset = (int64_t)sr->tag_bit_offset;

    IrLabel done_bb = irwriter_label(w);
    IrLabel fail_bb = irwriter_label(w);
    IrLabel material_parse = irwriter_label(w);

    if (memoize_mode == MEMOIZE_NONE) {
      irwriter_br(w, material_parse);
    } else if (memoize_mode == MEMOIZE_SHARED) {
      IrVal col_val = irwriter_load(w, "i64", col_ptr);
      IrVal row_off = irwriter_binop(w, "mul", "i64", col_val, irwriter_imm_int(w, (int)sizeof_col));
      IrVal slot_byte = irwriter_binop(
          w, "add", "i64", row_off, irwriter_imm_int(w, (int)(cl->bits_bucket_size * 8 + (int64_t)sr->slot_index * 4)));
      irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%r%d, i64 %%r%d\n", irwriter_next_reg(w), (int)peg_table,
                    (int)slot_byte);
      IrVal slot_ptr = (IrVal)(irwriter_next_reg(w) - 1);
      IrVal slot_val = irwriter_load(w, "i32", slot_ptr);
      IrVal is_cached = irwriter_icmp(w, "ne", "i32", slot_val, irwriter_imm_int(w, -1));

      uint64_t seg_off = sr->segment_index * 8;
      IrVal bt = irwriter_call_retf(w, "i1", "bit_test", "ptr %%r%d, i64 %%r%d, i64 %lld, i64 %llu, i64 %llu",
                                    (int)peg_table, (int)col_val, (long long)sizeof_col, (unsigned long long)seg_off,
                                    (unsigned long long)sr->rule_bit_mask);
      IrLabel bit_ok = irwriter_label(w);
      irwriter_br_cond(w, bt, bit_ok, fail_bb);

      irwriter_bb_at(w, bit_ok);
      IrLabel fast_ret = irwriter_label(w);
      irwriter_br_cond(w, is_cached, fast_ret, material_parse);
      irwriter_bb_at(w, fast_ret);
      IrVal cached = irwriter_sext(w, "i32", slot_val, "i64");
      irwriter_store(w, "i64", cached, parsed_tokens);
      irwriter_store(w, "i64", irwriter_binop(w, "add", "i64", col_val, cached), col_ptr);
      irwriter_br(w, done_bb);
    } else {
      IrVal col_val = irwriter_load(w, "i64", col_ptr);
      IrVal row_off = irwriter_binop(w, "mul", "i64", col_val, irwriter_imm_int(w, (int)sizeof_col));
      IrVal slot_byte = irwriter_binop(
          w, "add", "i64", row_off, irwriter_imm_int(w, (int)(cl->bits_bucket_size * 8 + (int64_t)sr->slot_index * 4)));
      irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%r%d, i64 %%r%d\n", irwriter_next_reg(w), (int)peg_table,
                    (int)slot_byte);
      IrVal slot_ptr = (IrVal)(irwriter_next_reg(w) - 1);
      IrVal slot_val = irwriter_load(w, "i32", slot_ptr);

      IrVal is_success = irwriter_icmp(w, "sge", "i32", slot_val, irwriter_imm_int(w, 0));
      IrLabel fast_ret = irwriter_label(w);
      IrLabel not_success = irwriter_label(w);
      irwriter_br_cond(w, is_success, fast_ret, not_success);

      irwriter_bb_at(w, fast_ret);
      IrVal cached = irwriter_sext(w, "i32", slot_val, "i64");
      irwriter_store(w, "i64", cached, parsed_tokens);
      irwriter_store(w, "i64", irwriter_binop(w, "add", "i64", col_val, cached), col_ptr);
      irwriter_br(w, done_bb);

      irwriter_bb_at(w, not_success);
      IrVal is_unknown = irwriter_icmp(w, "eq", "i32", slot_val, irwriter_imm_int(w, -1));
      irwriter_br_cond(w, is_unknown, material_parse, fail_bb);
    }

    irwriter_bb_at(w, material_parse);

    if (tag_size > 0) {
      irwriter_store(w, "i64", irwriter_imm_int(w, 0), tag_bits);
    }

    IrLabel parse_fail = irwriter_label(w);
    IrLabel parse_success = irwriter_label(w);

    peg_ir_emit_parse(&ctx, &sr->body, parse_fail);
    irwriter_br(w, parse_success);

    irwriter_bb_at(w, parse_success);
    IrVal start_col = irwriter_call_retf(w, "i64", "top", "ptr %%r%d", (int)stack_ptr);
    IrVal cur_col = irwriter_load(w, "i64", col_ptr);
    IrVal pt = irwriter_binop(w, "sub", "i64", cur_col, start_col);
    irwriter_store(w, "i64", pt, parsed_tokens);

    if (tag_size > 0) {
      IrVal bo = irwriter_binop(w, "mul", "i64", start_col, irwriter_imm_int(w, (int)sizeof_col));
      IrVal tbo = irwriter_binop(w, "add", "i64", bo, irwriter_imm_int(w, (int)(sr->tag_bit_index * 8)));
      irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%r%d, i64 %%r%d\n", irwriter_next_reg(w), (int)peg_table,
                    (int)tbo);
      IrVal bp = (IrVal)(irwriter_next_reg(w) - 1);
      IrVal old = irwriter_load(w, "i64", bp);
      irwriter_rawf(w, "  %%r%d = and i64 %%r%d, %llu\n", irwriter_next_reg(w), (int)old,
                    (unsigned long long)~sr->tag_bit_mask);
      IrVal cleared = (IrVal)(irwriter_next_reg(w) - 1);
      IrVal combined = irwriter_binop(w, "or", "i64", cleared, irwriter_load(w, "i64", tag_bits));
      irwriter_store(w, "i64", combined, bp);
    }
    if (memoize_mode == MEMOIZE_SHARED) {
      IrVal pt2 = irwriter_load(w, "i64", parsed_tokens);
      IrVal so = irwriter_binop(w, "mul", "i64", start_col, irwriter_imm_int(w, (int)sizeof_col));
      IrVal sbo = irwriter_binop(w, "add", "i64", so,
                                 irwriter_imm_int(w, (int)(cl->bits_bucket_size * 8 + (int64_t)sr->slot_index * 4)));
      irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%r%d, i64 %%r%d\n", irwriter_next_reg(w), (int)peg_table,
                    (int)sbo);
      IrVal wp = (IrVal)(irwriter_next_reg(w) - 1);
      irwriter_rawf(w, "  %%r%d = trunc i64 %%r%d to i32\n", irwriter_next_reg(w), (int)pt2);
      irwriter_store(w, "i32", (IrVal)(irwriter_next_reg(w) - 1), wp);
      irwriter_call_void_fmtf(w, "bit_exclude", "ptr %%r%d, i64 %%r%d, i64 %lld, i64 %llu, i64 %llu, i64 %llu",
                              (int)peg_table, (int)start_col, (long long)sizeof_col,
                              (unsigned long long)(sr->segment_index * 8), (unsigned long long)sr->segment_mask,
                              (unsigned long long)sr->rule_bit_mask);
    } else if (memoize_mode == MEMOIZE_NAIVE) {
      IrVal pt2 = irwriter_load(w, "i64", parsed_tokens);
      IrVal so = irwriter_binop(w, "mul", "i64", start_col, irwriter_imm_int(w, (int)sizeof_col));
      IrVal sbo = irwriter_binop(w, "add", "i64", so,
                                 irwriter_imm_int(w, (int)(cl->bits_bucket_size * 8 + (int64_t)sr->slot_index * 4)));
      irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%r%d, i64 %%r%d\n", irwriter_next_reg(w), (int)peg_table,
                    (int)sbo);
      IrVal wp = (IrVal)(irwriter_next_reg(w) - 1);
      irwriter_rawf(w, "  %%r%d = trunc i64 %%r%d to i32\n", irwriter_next_reg(w), (int)pt2);
      irwriter_store(w, "i32", (IrVal)(irwriter_next_reg(w) - 1), wp);
    }
    irwriter_br(w, done_bb);

    irwriter_bb_at(w, parse_fail);
    IrVal fail_col = irwriter_call_retf(w, "i64", "top", "ptr %%r%d", (int)stack_ptr);
    irwriter_store(w, "i64", fail_col, col_ptr);
    if (memoize_mode == MEMOIZE_SHARED) {
      irwriter_call_void_fmtf(w, "bit_deny", "ptr %%r%d, i64 %%r%d, i64 %lld, i64 %llu, i64 %llu", (int)peg_table,
                              (int)fail_col, (long long)sizeof_col, (unsigned long long)(sr->segment_index * 8),
                              (unsigned long long)sr->rule_bit_mask);
    } else if (memoize_mode == MEMOIZE_NAIVE) {
      IrVal so = irwriter_binop(w, "mul", "i64", fail_col, irwriter_imm_int(w, (int)sizeof_col));
      IrVal sbo = irwriter_binop(w, "add", "i64", so,
                                 irwriter_imm_int(w, (int)(cl->bits_bucket_size * 8 + (int64_t)sr->slot_index * 4)));
      irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%r%d, i64 %%r%d\n", irwriter_next_reg(w), (int)peg_table,
                    (int)sbo);
      IrVal wp = (IrVal)(irwriter_next_reg(w) - 1);
      irwriter_store(w, "i32", irwriter_imm_int(w, -2), wp);
    }
    irwriter_br(w, fail_bb);

    irwriter_bb_at(w, fail_bb);
    irwriter_store(w, "i64", irwriter_imm_int(w, -1), parsed_tokens);
    irwriter_br(w, done_bb);

    irwriter_bb_at(w, done_bb);
    peg_ir_emit_ret(&ctx);
  }

  irwriter_bb_at(w, final_ret);
  IrVal fr = irwriter_load(w, "i64", parse_result);
  IrVal fc = irwriter_load(w, "i64", col_ptr);
  IrVal r0 = irwriter_insertvalue(w, "{i64, i64}", -1, "i64", fr, 0);
  IrVal r1 = irwriter_insertvalue(w, "{i64, i64}", r0, "i64", fc, 1);
  irwriter_ret(w, "{i64, i64}", r1);
  irwriter_define_end(w);
  darray_del(ctx.ret_labels);
  free(fn_name);
}

// ============================================================
// Public API
// ============================================================

void peg_gen(PegGenInput* input, HeaderWriter* hw, IrWriter* w) {
  ScopeClosure* closures = input->scope_closures;
  int32_t closure_size = (int32_t)darray_size(closures);
  if (closure_size == 0) {
    return;
  }

  peg_ir_emit_helpers(w);
  irwriter_type_def(w, "Token", "{i32, i32, i32, i32}");
  if (input->memoize_mode == MEMOIZE_SHARED) {
    peg_ir_emit_bit_helpers(w);
  }
  for (int32_t c = 0; c < closure_size; c++) {
    if (input->verbose) {
      fprintf(stderr, "  [peg] generating IR for scope '%s'\n", closures[c].scope_name);
    }
    _gen_scope_ir(w, &closures[c], input->memoize_mode);
  }

  RuleScopeMap scope_map = _build_scope_map(input);
  _gen_header(input, hw, &scope_map);
  _free_scope_map(&scope_map);
}
