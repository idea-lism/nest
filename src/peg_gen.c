// specs/peg_gen.md
#include "darray.h"
#include "peg.h"
#include "peg_ir.h"
#include "symtab.h"
#include "xmalloc.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ============================================================
// Helpers
// ============================================================

__attribute__((format(printf, 1, 2))) static char* _asprintf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char* buf = XMALLOC((size_t)n + 1);
  va_start(ap, fmt);
  vsnprintf(buf, (size_t)n + 1, fmt, ap);
  va_end(ap);
  return buf;
}

// Round slots_size up to even so sizeof_col is always 8-byte aligned
static int64_t _aligned_slots_size(int64_t slots_size) { return (slots_size + 1) & ~(int64_t)1; }

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
  char* out = XMALLOC(len + 2);
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

static void _gen_node_struct(HeaderWriter* hw, ScopedRule* sr, const char* rule_name) {
  if (!sr->node_fields) {
    return;
  }
  int32_t tag_size = symtab_count(&sr->tags);
  hdwriter_printf(hw, "typedef struct {\n");
  if (tag_size > 0) {
    hdwriter_puts(hw, "  struct {\n");
    for (int32_t tag_idx = 0; tag_idx < tag_size; tag_idx++) {
      char* sanitized_name = _sanitize_field_name(symtab_get(&sr->tags, tag_idx + sr->tags.start_num));
      hdwriter_printf(hw, "    bool %s : 1;\n", sanitized_name);
      XFREE(sanitized_name);
    }
    int32_t n_buckets = (tag_size + 63) / 64;
    for (int32_t bi = 0; bi < n_buckets; bi++) {
      int32_t bits_in_bucket = (bi < n_buckets - 1) ? 64 : tag_size - bi * 64;
      int32_t padding = 64 - bits_in_bucket;
      if (padding > 0) {
        if (n_buckets == 1) {
          hdwriter_printf(hw, "    uint64_t _padding : %d;\n", padding);
        } else {
          hdwriter_printf(hw, "    uint64_t _padding%d : %d;\n", bi, padding);
        }
      }
    }
    hdwriter_puts(hw, "  } is;\n");
  }
  for (size_t i = 0; i < darray_size(sr->node_fields); i++) {
    NodeField* nf = &sr->node_fields[i];
    hdwriter_printf(hw, "  %s %s;\n", nf->is_link ? "PegLink" : "PegRef", nf->name);
  }
  hdwriter_printf(hw, "} Node_%s;\n", rule_name);
  hdwriter_putc(hw, '\n');
}

static int32_t _compute_slot_row(ScopeClosure* cl, ScopedRule* sr) {
  return (int32_t)(cl->bits_bucket_size * 2 + (int64_t)sr->slot_index);
}

static ScopedRule* _find_scoped_rule_by_name(ScopeClosure* cl, const char* name) {
  int32_t id = symtab_find(&cl->scoped_rule_names, name);
  if (id < 0) {
    return NULL;
  }
  return &cl->scoped_rules[id - cl->scoped_rule_names.start_num];
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
  size_t closure_size = darray_size(closures);
  RuleScopeMap map = {
      .start_num = input->rule_names.start_num,
      .size = symtab_count(&input->rule_names),
  };
  map.entries = XCALLOC((size_t)map.size, sizeof(RuleScopeEntry*));
  map.counts = XCALLOC((size_t)map.size, sizeof(int32_t));
  for (size_t c = 0; c < closure_size; c++) {
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
      int32_t entry_count = map.counts[idx];
      map.entries[idx] = XREALLOC(map.entries[idx], (size_t)(entry_count + 1) * sizeof(RuleScopeEntry));
      map.entries[idx][entry_count] = (RuleScopeEntry){.scope_id = cl->scope_id, .sr = sr, .cl = cl};
      map.counts[idx] = entry_count + 1;
    }
  }
  return map;
}

static void _free_scope_map(RuleScopeMap* map) {
  for (int32_t i = 0; i < map->size; i++) {
    XFREE(map->entries[i]);
  }
  XFREE(map->entries);
  XFREE(map->counts);
}

static void _gen_loader(HeaderWriter* hw, const char* rule_name, RuleScopeEntry* scope_entries,
                        int32_t used_in_closures_count, const char* prefix) {
  if (used_in_closures_count == 0) {
    return;
  }
  ScopedRule* first_scoped_rule = scope_entries[0].sr;
  if (!first_scoped_rule->node_fields) {
    return;
  }
  int32_t tag_size = symtab_count(&first_scoped_rule->tags);
  bool has_tags = (tag_size > 0);
  hdwriter_printf(hw, "static Node_%s %s_load_%s(PegRef ref)", rule_name, prefix, rule_name);
  hdwriter_begin(hw);
  hdwriter_printf(hw, "Node_%s $1 = {0};\n", rule_name);
  if (has_tags) {
    hdwriter_puts(hw, "int64_t* $table = (int64_t*)ref.tc->value;\n");
  }
  hdwriter_puts(hw, "switch (ref.tc->scope_id)");
  hdwriter_begin(hw);
  for (int32_t entry_idx = 0; entry_idx < used_in_closures_count; entry_idx++) {
    ScopedRule* sr = scope_entries[entry_idx].sr;
    ScopeClosure* cl = scope_entries[entry_idx].cl;
    int64_t col_size_in_i32 = cl->bits_bucket_size * 2 + _aligned_slots_size(cl->slots_size);
    hdwriter_printf(hw, "case %d:", scope_entries[entry_idx].scope_id);
    hdwriter_begin(hw);
    if (has_tags) {
      hdwriter_printf(hw, "int64_t* $col = (int64_t*)((int32_t*)$table + %lld * ref.col);\n",
                      (long long)col_size_in_i32);
      if (tag_size <= 64) {
        hdwriter_printf(hw, "((uint64_t*)&$1.is)[0] = ($col[%d] >> %dULL) & 0x%llxULL;\n", sr->tag_bit_index,
                        sr->tag_bit_offset, (unsigned long long)_tag_mask(tag_size, 0));
      } else {
        // Big: dedicated buckets, offset=0, copy consecutive words
        int32_t n_buckets = (tag_size + 63) / 64;
        for (int32_t bi = 0; bi < n_buckets; bi++) {
          hdwriter_printf(hw, "((uint64_t*)&$1.is)[%d] = $col[%d];\n", bi, sr->tag_bit_index + bi);
        }
      }
    }
    int32_t current_branch = -1;
    for (size_t field_idx = 0; field_idx < darray_size(sr->node_fields); field_idx++) {
      NodeField* nf = &sr->node_fields[field_idx];
      // Branch grouping: close previous branch block if switching
      if (current_branch >= 0 && nf->branch_index != current_branch) {
        hdwriter_end(hw);
        current_branch = -1;
      }
      // Open new branch block if needed
      if (nf->branch_index >= 0 && nf->branch_index != current_branch) {
        current_branch = nf->branch_index;
        const char* tag_name = symtab_get(&sr->tags, sr->tags.start_num + current_branch);
        char* san_tag = _sanitize_field_name(tag_name);
        hdwriter_printf(hw, "if ($1.is.%s)", san_tag);
        XFREE(san_tag);
        hdwriter_begin(hw);
      }
      if (nf->is_link) {
        if (nf->is_scope) {
          hdwriter_printf(hw, "$1.%s.tc = &((TokenTree*)ref.tc->aux_value)->table[ref.tc->tokens[ref.col].chunk_id];\n",
                          nf->name);
          hdwriter_printf(hw, "$1.%s.col = 0;\n", nf->name);
        } else {
          hdwriter_printf(hw, "$1.%s.tc = ref.tc;\n", nf->name);
          hdwriter_printf(hw, "$1.%s.col = ref.col;\n", nf->name);
        }
        hdwriter_printf(hw, "$1.%s.col_size_in_i32 = %lld;\n", nf->name, (long long)col_size_in_i32);
        // find wrapper scoped rule and extract LHS/RHS info
        ScopedRule* wrapper = nf->wrapper_name ? _find_scoped_rule_by_name(cl, nf->wrapper_name) : NULL;
        ScopedUnit* body = wrapper ? &wrapper->body : NULL;
        ScopedUnit* lhs = NULL;
        if (body && (body->kind == SCOPED_UNIT_STAR || body->kind == SCOPED_UNIT_PLUS)) {
          lhs = body->as.interlace.lhs;
        } else if (body && body->kind == SCOPED_UNIT_MAYBE) {
          lhs = body->as.base;
        }
        if (lhs && lhs->kind == SCOPED_UNIT_TERM) {
          // term LHS: has_elem checks term_id, get_next advances by 1
          hdwriter_printf(hw, "$1.%s.lhs_bit_index = 0; $1.%s.lhs_bit_mask = 0; $1.%s.lhs_row = %d;\n", nf->name,
                          nf->name, nf->name, PEG_ROW_TERM);
          hdwriter_printf(hw, "$1.%s.lhs_term_id = %d;\n", nf->name, lhs->as.term_id);
        } else if (lhs && lhs->kind == SCOPED_UNIT_CALL) {
          ScopedRule* callee = _find_scoped_rule_by_name(cl, lhs->as.callee);
          int32_t lhs_row = callee ? _compute_slot_row(cl, callee) : 0;
          hdwriter_printf(hw, "$1.%s.lhs_row = %d;\n", nf->name, lhs_row);
          hdwriter_printf(hw, "$1.%s.lhs_term_id = 0;\n", nf->name);
          hdwriter_printf(hw, "$1.%s.lhs_bit_index = %llu; $1.%s.lhs_bit_mask = 0x%llxULL;\n", nf->name,
                          callee ? (unsigned long long)callee->segment_index : 0ULL, nf->name,
                          callee ? (unsigned long long)callee->rule_bit_mask : 0ULL);
        } else {
          hdwriter_printf(hw, "$1.%s.lhs_bit_index = 0; $1.%s.lhs_bit_mask = 0; $1.%s.lhs_row = %d;\n", nf->name,
                          nf->name, nf->name, PEG_ROW_NONE);
          hdwriter_printf(hw, "$1.%s.lhs_term_id = 0;\n", nf->name);
        }
        // RHS info for interlaced links
        if (nf->rhs_row == PEG_ROW_TERM) {
          hdwriter_printf(hw, "$1.%s.rhs_row = %d; $1.%s.rhs_bit_index = 0; $1.%s.rhs_bit_mask = 0;\n", nf->name,
                          PEG_ROW_TERM, nf->name, nf->name);
        } else if (nf->rhs_row >= 0 && body && (body->kind == SCOPED_UNIT_STAR || body->kind == SCOPED_UNIT_PLUS)) {
          ScopedUnit* rhs = body->as.interlace.rhs;
          ScopedRule* rhs_sr =
              (rhs && rhs->kind == SCOPED_UNIT_CALL) ? _find_scoped_rule_by_name(cl, rhs->as.callee) : NULL;
          int32_t rhs_row = rhs_sr ? _compute_slot_row(cl, rhs_sr) : nf->rhs_row;
          hdwriter_printf(hw, "$1.%s.rhs_row = %d;\n", nf->name, rhs_row);
          hdwriter_printf(hw, "$1.%s.rhs_bit_index = %llu; $1.%s.rhs_bit_mask = 0x%llxULL;\n", nf->name,
                          rhs_sr ? (unsigned long long)rhs_sr->segment_index : 0ULL, nf->name,
                          rhs_sr ? (unsigned long long)rhs_sr->rule_bit_mask : 0ULL);
        } else {
          hdwriter_printf(hw, "$1.%s.rhs_row = %d; $1.%s.rhs_bit_index = 0; $1.%s.rhs_bit_mask = 0;\n", nf->name,
                          PEG_ROW_NONE, nf->name, nf->name);
        }
      } else {
        if (nf->is_scope) {
          hdwriter_printf(hw, "$1.%s.tc = &((TokenTree*)ref.tc->aux_value)->table[ref.tc->tokens[ref.col].chunk_id];\n",
                          nf->name);
          hdwriter_printf(hw, "$1.%s.col = 0;\n", nf->name);
          hdwriter_printf(hw, "$1.%s.row = %d;\n", nf->name, nf->ref_row);
        } else {
          hdwriter_printf(hw, "$1.%s.tc = ref.tc;\n", nf->name);
          hdwriter_printf(hw, "$1.%s.col = ref.col;\n", nf->name);
          hdwriter_printf(hw, "$1.%s.row = %d;\n", nf->name, nf->ref_row);
        }
      }
      switch (nf->advance) {
      case NODE_ADVANCE_ONE:
        hdwriter_puts(hw, "ref.col++;\n");
        break;
      case NODE_ADVANCE_SLOT:
        hdwriter_printf(hw, "ref.col += ((int32_t*)ref.tc->value)[ref.col * %lld + %d];\n", (long long)col_size_in_i32,
                        nf->advance_slot_row);
        break;
      case NODE_ADVANCE_NONE:
        break;
      }
    }
    // Close any open branch block
    if (current_branch >= 0) {
      hdwriter_end(hw);
    }

    hdwriter_puts(hw, "break;\n");
    hdwriter_end(hw);
  }
  hdwriter_end(hw);
  hdwriter_puts(hw, "return $1;\n");
  hdwriter_end(hw);
  hdwriter_putc(hw, '\n');
}

// Forward-declare darray_size since iteration helpers appear before runtime
static void _gen_iter_preamble(HeaderWriter* hw) { (void)hw; }
static void _gen_has_elem(HeaderWriter* hw, const char* prefix, int memoize_mode) {
  hdwriter_printf(hw, "static bool %s_has_elem(PegLink* l)", prefix);
  hdwriter_begin(hw);
  // sentinel token/column eliminates bounds checks (see token_tree.md#sentinel-token)
  // term LHS: sentinel token has term_id=0, never matches any valid term
  hdwriter_puts(hw, "if (l->lhs_row == -2) return l->tc->tokens[l->col].term_id == (int32_t)l->lhs_term_id;\n");
  // call LHS: sentinel memoize column is zero-initialized, bits test fails, slot reads as 0
  hdwriter_puts(hw, "int32_t* $col = (int32_t*)l->tc->value + l->col_size_in_i32 * l->col;\n");
  if (memoize_mode == MEMOIZE_SHARED) {
    hdwriter_puts(hw, "uint64_t* $bits = (uint64_t*)$col;\n");
    hdwriter_puts(hw, "return ($bits[l->lhs_bit_index] & l->lhs_bit_mask) && $col[l->lhs_row] >= 0;\n");
  } else {
    hdwriter_puts(hw, "return $col[l->lhs_row] >= 0;\n");
  }
  hdwriter_end(hw);
  hdwriter_putc(hw, '\n');
}
static void _gen_get_next(HeaderWriter* hw, const char* prefix, int memoize_mode) {
  hdwriter_printf(hw, "static void %s_get_next(PegLink* l)", prefix);
  hdwriter_begin(hw);
  // LHS advance: if lhs_row == -2 it's a term (advance by 1), else read slot
  hdwriter_puts(hw, "if (l->lhs_row == -2) { l->col += 1; }\n");
  hdwriter_puts(hw, "else");
  hdwriter_begin(hw);
  hdwriter_puts(hw, "int32_t* $col = (int32_t*)l->tc->value + l->col_size_in_i32 * l->col;\n");
  hdwriter_puts(hw, "l->col += $col[l->lhs_row];\n");
  hdwriter_end(hw);
  // RHS advance: sentinel column naturally stops advancement (slot reads as 0)
  hdwriter_puts(hw, "if (l->rhs_row == -2)");
  hdwriter_begin(hw);
  hdwriter_puts(hw, "l->col += 1;\n");
  hdwriter_end(hw);
  hdwriter_puts(hw, "else if (l->rhs_row >= 0)");
  hdwriter_begin(hw);
  hdwriter_puts(hw, "int32_t* $rhs_col = (int32_t*)l->tc->value + l->col_size_in_i32 * l->col;\n");
  if (memoize_mode == MEMOIZE_SHARED) {
    hdwriter_puts(hw, "uint64_t* $rhs_bits = (uint64_t*)$rhs_col;\n");
    hdwriter_puts(hw, "if (($rhs_bits[l->rhs_bit_index] & l->rhs_bit_mask) && $rhs_col[l->rhs_row] > 0) "
                      "l->col += $rhs_col[l->rhs_row];\n");
  } else {
    hdwriter_puts(hw, "if ($rhs_col[l->rhs_row] > 0) l->col += $rhs_col[l->rhs_row];\n");
  }
  hdwriter_end(hw);
  hdwriter_end(hw);
  hdwriter_putc(hw, '\n');
}
static void _gen_get_lhs(HeaderWriter* hw, const char* prefix) {
  hdwriter_printf(hw, "static PegRef %s_get_lhs(PegLink* l)", prefix);
  hdwriter_begin(hw);
  hdwriter_puts(hw, "return (PegRef){l->tc, l->col, l->lhs_row};\n");
  hdwriter_end(hw);
  hdwriter_putc(hw, '\n');
}
static void _gen_get_rhs(HeaderWriter* hw, const char* prefix) {
  hdwriter_printf(hw, "static PegRef %s_get_rhs(PegLink* l)", prefix);
  hdwriter_begin(hw);
  hdwriter_puts(hw, "if (l->lhs_row == -2) return (PegRef){l->tc, l->col + 1, l->rhs_row};\n");
  hdwriter_puts(hw, "int32_t* $col = (int32_t*)l->tc->value + l->col_size_in_i32 * l->col;\n");
  hdwriter_puts(hw, "return (PegRef){l->tc, l->col + $col[l->lhs_row], l->rhs_row};\n");
  hdwriter_end(hw);
  hdwriter_putc(hw, '\n');
}
static void _gen_peg_size(HeaderWriter* hw, ScopeClosure* closures, int32_t closure_size, const char* prefix) {
  hdwriter_printf(hw, "static int64_t %s_peg_size(PegRef ref)", prefix);
  hdwriter_begin(hw);
  hdwriter_puts(hw, "if (!ref.tc || !ref.tc->value) return -1;\n");
  // sentinel column reads as 0 (no match), no explicit bounds check needed
  hdwriter_puts(hw, "switch (ref.tc->scope_id)");
  hdwriter_begin(hw);
  for (int32_t c = 0; c < closure_size; c++) {
    ScopeClosure* cl = &closures[c];
    int64_t col_size_in_i32 = cl->bits_bucket_size * 2 + _aligned_slots_size(cl->slots_size);
    hdwriter_printf(hw, "case %d: return ((int32_t*)ref.tc->value)[ref.col * %lld + ref.row];\n", cl->scope_id,
                    (long long)col_size_in_i32);
  }
  hdwriter_puts(hw, "default: return -1;\n");
  hdwriter_end(hw);
  hdwriter_end(hw);
  hdwriter_putc(hw, '\n');
}

static void _gen_header(PegGenInput* input, HeaderWriter* hw, RuleScopeMap* scope_map) {
  ScopeClosure* closures = input->scope_closures;
  int32_t closure_size = (int32_t)darray_size(closures);
  const char* prefix = input->prefix;

  // emit node structs and loaders for each distinct global rule
  for (int32_t gid = 0; gid < scope_map->size; gid++) {
    if (scope_map->counts[gid] == 0) {
      continue;
    }
    const char* rule_name = symtab_get(&input->rule_names, gid + scope_map->start_num);
    _gen_node_struct(hw, scope_map->entries[gid][0].sr, rule_name);
    _gen_loader(hw, rule_name, scope_map->entries[gid], scope_map->counts[gid], prefix);
  }

  _gen_iter_preamble(hw);
  _gen_has_elem(hw, prefix, input->memoize_mode);
  _gen_get_next(hw, prefix, input->memoize_mode);
  _gen_get_lhs(hw, prefix);
  _gen_get_rhs(hw, prefix);
  _gen_peg_size(hw, closures, closure_size, prefix);
}

// ============================================================
// LLVM IR generation — per-scope context
// ============================================================

static IrVal _tag_alloca(PegIrCtx* ctx, int32_t n) {
  if (n == 0) {
    return ctx->tag_bits;
  }
  return ctx->tag_bits_extra[n - 1];
}

typedef struct {
  IrVal peg_table;
  int64_t sizeof_col;
  int64_t bits_bucket_size;
  int64_t slot_byte_offset; // precomputed: bits_bucket_size * 8 + slot_index * 4
  int64_t tag_byte_offset;  // precomputed: tag_bit_index * 8
  int32_t tag_bit_count;    // forwarded from ScopedRule
} PegIrScopeCtx;

// ============================================================
// Memoize read strategies
// ============================================================

typedef struct {
  IrLabel material_parse;
  IrLabel fail_bb;
  IrLabel done_bb;
} MemoizeLabels;

static void _memoize_read_none(PegIrCtx* ctx, PegIrScopeCtx* sc, ScopedRule* sr, MemoizeLabels* ml) {
  (void)sc;
  (void)sr;
  irwriter_br(ctx->ir_writer, ml->material_parse);
}

static void _memoize_read_shared(PegIrCtx* ctx, PegIrScopeCtx* sc, ScopedRule* sr, MemoizeLabels* ml) {
  IrWriter* w = ctx->ir_writer;
  IrVal col_val = irwriter_load(w, "i64", ctx->col);

  // sentinel column is zero-initialized: bit_test returns false → branches to fail
  IrVal slot_ptr =
      irwriter_call_retf(w, "ptr", "gep_slot", "ptr %%r%d, i64 %%r%d, i64 %lld, i64 %lld", (int)sc->peg_table,
                         (int)col_val, (long long)sc->sizeof_col, (long long)sc->slot_byte_offset);
  IrVal slot_val = irwriter_load(w, "i32", slot_ptr);
  IrVal is_cached = irwriter_icmp(w, "ne", "i32", slot_val, irwriter_imm_int(w, -1));

  uint64_t seg_off = sr->segment_index * 8;
  IrVal bit_test_result = irwriter_call_retf(w, "i1", "bit_test", "ptr %%r%d, i64 %%r%d, i64 %lld, i64 %llu, i64 %llu",
                                             (int)sc->peg_table, (int)col_val, (long long)sc->sizeof_col,
                                             (unsigned long long)seg_off, (unsigned long long)sr->rule_bit_mask);
  IrLabel bit_ok = irwriter_label(w);
  irwriter_br_cond(w, bit_test_result, bit_ok, ml->fail_bb);

  irwriter_bb_at(w, bit_ok);
  IrLabel fast_ret = irwriter_label(w);
  irwriter_br_cond(w, is_cached, fast_ret, ml->material_parse);
  irwriter_bb_at(w, fast_ret);
  IrVal cached = irwriter_sext(w, "i32", slot_val, "i64");
  irwriter_store(w, "i64", cached, ctx->parsed_tokens);
  irwriter_store(w, "i64", irwriter_binop(w, "add", "i64", col_val, cached), ctx->col);
  irwriter_br(w, ml->done_bb);
}

static void _memoize_read_naive(PegIrCtx* ctx, PegIrScopeCtx* sc, ScopedRule* sr, MemoizeLabels* ml) {
  (void)sr;
  IrWriter* w = ctx->ir_writer;
  IrVal col_val = irwriter_load(w, "i64", ctx->col);

  // sentinel column is zero-initialized: slot reads as 0, is_success true, fast_ret with 0 tokens parsed
  // but term match at sentinel fails (term_id=0), so parsing stops correctly
  IrVal slot_ptr =
      irwriter_call_retf(w, "ptr", "gep_slot", "ptr %%r%d, i64 %%r%d, i64 %lld, i64 %lld", (int)sc->peg_table,
                         (int)col_val, (long long)sc->sizeof_col, (long long)sc->slot_byte_offset);
  IrVal slot_val = irwriter_load(w, "i32", slot_ptr);

  IrVal is_success = irwriter_icmp(w, "sge", "i32", slot_val, irwriter_imm_int(w, 0));
  IrLabel fast_ret = irwriter_label(w);
  IrLabel not_success = irwriter_label(w);
  irwriter_br_cond(w, is_success, fast_ret, not_success);

  irwriter_bb_at(w, fast_ret);
  IrVal cached = irwriter_sext(w, "i32", slot_val, "i64");
  irwriter_store(w, "i64", cached, ctx->parsed_tokens);
  irwriter_store(w, "i64", irwriter_binop(w, "add", "i64", col_val, cached), ctx->col);
  irwriter_br(w, ml->done_bb);

  irwriter_bb_at(w, not_success);
  IrVal is_unknown = irwriter_icmp(w, "eq", "i32", slot_val, irwriter_imm_int(w, -1));
  irwriter_br_cond(w, is_unknown, ml->material_parse, ml->fail_bb);
}

// ============================================================
// Memoize write strategies (cache on success)
// ============================================================

static void _memoize_write_shared(PegIrCtx* ctx, PegIrScopeCtx* sc, ScopedRule* sr, IrVal start_col) {
  IrWriter* w = ctx->ir_writer;
  IrVal parsed_tokens_val = irwriter_load(w, "i64", ctx->parsed_tokens);
  IrVal slot_ptr =
      irwriter_call_retf(w, "ptr", "gep_slot", "ptr %%r%d, i64 %%r%d, i64 %lld, i64 %lld", (int)sc->peg_table,
                         (int)start_col, (long long)sc->sizeof_col, (long long)sc->slot_byte_offset);
  irwriter_rawf(w, "  %%r%d = trunc i64 %%r%d to i32\n", irwriter_next_reg(w), (int)parsed_tokens_val);
  irwriter_store(w, "i32", (IrVal)(irwriter_next_reg(w) - 1), slot_ptr);
  irwriter_call_void_fmtf(w, "bit_exclude", "ptr %%r%d, i64 %%r%d, i64 %lld, i64 %llu, i64 %llu, i64 %llu",
                          (int)sc->peg_table, (int)start_col, (long long)sc->sizeof_col,
                          (unsigned long long)(sr->segment_index * 8), (unsigned long long)sr->segment_mask,
                          (unsigned long long)sr->rule_bit_mask);
}

static void _memoize_write_naive(PegIrCtx* ctx, PegIrScopeCtx* sc, ScopedRule* sr, IrVal start_col) {
  (void)sr;
  IrWriter* w = ctx->ir_writer;
  IrVal parsed_tokens_val = irwriter_load(w, "i64", ctx->parsed_tokens);
  IrVal slot_ptr =
      irwriter_call_retf(w, "ptr", "gep_slot", "ptr %%r%d, i64 %%r%d, i64 %lld, i64 %lld", (int)sc->peg_table,
                         (int)start_col, (long long)sc->sizeof_col, (long long)sc->slot_byte_offset);
  irwriter_rawf(w, "  %%r%d = trunc i64 %%r%d to i32\n", irwriter_next_reg(w), (int)parsed_tokens_val);
  irwriter_store(w, "i32", (IrVal)(irwriter_next_reg(w) - 1), slot_ptr);
}

// ============================================================
// Memoize deny strategies (cache on failure)
// ============================================================

static void _memoize_deny_none(PegIrCtx* ctx, PegIrScopeCtx* sc, ScopedRule* sr, IrVal fail_col) {
  (void)ctx;
  (void)sc;
  (void)sr;
  (void)fail_col;
}

static void _memoize_deny_shared(PegIrCtx* ctx, PegIrScopeCtx* sc, ScopedRule* sr, IrVal fail_col) {
  IrWriter* w = ctx->ir_writer;
  irwriter_call_void_fmtf(w, "bit_deny", "ptr %%r%d, i64 %%r%d, i64 %lld, i64 %llu, i64 %llu", (int)sc->peg_table,
                          (int)fail_col, (long long)sc->sizeof_col, (unsigned long long)(sr->segment_index * 8),
                          (unsigned long long)sr->rule_bit_mask);
}

static void _memoize_deny_naive(PegIrCtx* ctx, PegIrScopeCtx* sc, ScopedRule* sr, IrVal fail_col) {
  (void)sr;
  IrWriter* w = ctx->ir_writer;
  IrVal slot_ptr =
      irwriter_call_retf(w, "ptr", "gep_slot", "ptr %%r%d, i64 %%r%d, i64 %lld, i64 %lld", (int)sc->peg_table,
                         (int)fail_col, (long long)sc->sizeof_col, (long long)sc->slot_byte_offset);
  irwriter_store(w, "i32", irwriter_imm_int(w, -2), slot_ptr);
}

// ============================================================
// Strategy dispatch tables
// ============================================================

typedef void (*MemoizeReadFn)(PegIrCtx*, PegIrScopeCtx*, ScopedRule*, MemoizeLabels*);
typedef void (*MemoizeWriteFn)(PegIrCtx*, PegIrScopeCtx*, ScopedRule*, IrVal);
typedef void (*MemoizeDenyFn)(PegIrCtx*, PegIrScopeCtx*, ScopedRule*, IrVal);

static MemoizeReadFn _memoize_read_fns[] = {
    [MEMOIZE_NONE] = _memoize_read_none,
    [MEMOIZE_NAIVE] = _memoize_read_naive,
    [MEMOIZE_SHARED] = _memoize_read_shared,
};

static MemoizeWriteFn _memoize_write_fns[] = {
    [MEMOIZE_NONE] = _memoize_write_naive,
    [MEMOIZE_NAIVE] = _memoize_write_naive,
    [MEMOIZE_SHARED] = _memoize_write_shared,
};

static MemoizeDenyFn _memoize_deny_fns[] = {
    [MEMOIZE_NONE] = _memoize_deny_none,
    [MEMOIZE_NAIVE] = _memoize_deny_naive,
    [MEMOIZE_SHARED] = _memoize_deny_shared,
};

// ============================================================
// _gen_scope_ir — main IR generation per scope
// ============================================================

static void _gen_scope_ir(IrWriter* w, ScopeClosure* cl, int memoize_mode) {
  int32_t rule_size = (int32_t)darray_size(cl->scoped_rules);
  if (rule_size == 0) {
    return;
  }
  int64_t sizeof_col = cl->bits_bucket_size * 8 + _aligned_slots_size(cl->slots_size) * 4;

  irwriter_declare(w, "ptr", "tt_current", "ptr");
  irwriter_declare(w, "ptr", "tt_alloc_memoize_table", "ptr, i64");

  char* fn_name = _asprintf("parse_%s", cl->scope_name);
  irwriter_define_startf(w, fn_name, "{i64, i64} @%s(ptr %%tt, ptr %%stack_ptr_in) sspreq", fn_name);
  irwriter_bb(w);
  irwriter_dbg(w, cl->source_line, cl->source_col);

  IrVal tc = irwriter_call_retf(w, "ptr", "tt_current", "ptr %%tt");
  IrVal peg_table =
      irwriter_call_retf(w, "ptr", "tt_alloc_memoize_table", "ptr %%r%d, i64 %lld", (int)tc, (long long)sizeof_col);

  irwriter_raw(w, "  %col = alloca i64\n");
  IrVal col = irwriter_imm(w, "%col");
  irwriter_store(w, "i64", irwriter_imm_int(w, 0), col);
  IrVal stack_ptr = irwriter_alloca(w, "ptr");
  irwriter_store(w, "ptr", irwriter_imm(w, "%stack_ptr_in"), stack_ptr);
  IrVal parse_result = irwriter_alloca(w, "i64");
  irwriter_store(w, "i64", irwriter_imm_int(w, -1), parse_result);
  IrVal parsed_tokens = irwriter_alloca(w, "i64");
  irwriter_store(w, "i64", irwriter_imm_int(w, 0), parsed_tokens);
  // Compute max tag bucket count across all rules in scope
  int32_t max_tag_buckets = 0;
  for (int32_t i = 0; i < rule_size; i++) {
    int32_t nt = symtab_count(&cl->scoped_rules[i].tags);
    int32_t nb = (nt > 0) ? (nt + 63) / 64 : 0;
    if (nb > max_tag_buckets) {
      max_tag_buckets = nb;
    }
  }
  if (max_tag_buckets < 1) {
    max_tag_buckets = 1;
  }

  irwriter_raw(w, "  %tag_bits = alloca i64\n");
  IrVal tag_bits = irwriter_imm(w, "%tag_bits");
  irwriter_store(w, "i64", irwriter_imm_int(w, 0), tag_bits);

  IrVal* tag_bits_extra = NULL;
  if (max_tag_buckets > 1) {
    tag_bits_extra = XMALLOC((size_t)(max_tag_buckets - 1) * sizeof(IrVal));
    for (int32_t bi = 1; bi < max_tag_buckets; bi++) {
      irwriter_rawf(w, "  %%tag_bits_%d = alloca i64\n", bi);
      char name_buf[32];
      snprintf(name_buf, sizeof(name_buf), "%%tag_bits_%d", bi);
      tag_bits_extra[bi - 1] = irwriter_imm(w, name_buf);
      irwriter_store(w, "i64", irwriter_imm_int(w, 0), tag_bits_extra[bi - 1]);
    }
  }

  irwriter_rawf(w, "  %%r%d = getelementptr i8, ptr %%r%d, i64 24\n", irwriter_next_reg(w), (int)tc);
  IrVal tokens = irwriter_load(w, "ptr", (IrVal)(irwriter_next_reg(w) - 1));

  PegIrCtx ctx = {
      .ir_writer = w,
      .fn_name = fn_name,
      .tc = tc,
      .tokens = tokens,
      .col = col,
      .stack_ptr = stack_ptr,
      .parse_result = parse_result,
      .tag_bits = tag_bits,
      .tag_bits_extra = tag_bits_extra,
      .tag_bits_n = max_tag_buckets,
      .parsed_tokens = parsed_tokens,
      .current_rule_id = -1,
      .call_site_counter = 0,
      .current_rule_call_sites = NULL,
  };

  MemoizeReadFn memoize_read = _memoize_read_fns[memoize_mode];
  MemoizeWriteFn memoize_write = _memoize_write_fns[memoize_mode];
  MemoizeDenyFn memoize_deny = _memoize_deny_fns[memoize_mode];

  IrLabel final_ret = peg_ir_emit_call(&ctx, cl->scoped_rules[0].scoped_rule_name);

  for (int32_t i = 0; i < rule_size; i++) {
    ScopedRule* sr = &cl->scoped_rules[i];
    irwriter_rawf(w, "\n%s:\n", sr->scoped_rule_name);

    int32_t tag_size = symtab_count(&sr->tags);
    ctx.tag_bit_offset = (int32_t)sr->tag_bit_offset;
    ctx.tag_bit_count = sr->tag_bit_count;
    ctx.current_rule_id = i;
    ctx.call_site_counter = 0;
    ctx.current_rule_call_sites = sr->call_sites;
    ctx.has_tags = tag_size > 0;

    PegIrScopeCtx sc = {
        .peg_table = peg_table,
        .sizeof_col = sizeof_col,
        .bits_bucket_size = cl->bits_bucket_size,
        .slot_byte_offset = cl->bits_bucket_size * 8 + (int64_t)sr->slot_index * 4,
        .tag_byte_offset = (int64_t)sr->tag_bit_index * 8,
        .tag_bit_count = sr->tag_bit_count,
    };

    MemoizeLabels ml = {
        .done_bb = irwriter_label(w),
        .fail_bb = irwriter_label(w),
        .material_parse = irwriter_label(w),
    };

    // --- memoize read ---
    memoize_read(&ctx, &sc, sr, &ml);

    // --- material parse ---
    irwriter_bb_at(w, ml.material_parse);
    if (tag_size > 0) {
      int32_t n_buckets = (tag_size + 63) / 64;
      for (int32_t bi = 0; bi < n_buckets; bi++) {
        irwriter_store(w, "i64", irwriter_imm_int(w, 0), _tag_alloca(&ctx, bi));
      }
    }

    IrLabel parse_fail = irwriter_label(w);
    IrLabel parse_success = irwriter_label(w);

    peg_ir_emit_parse(&ctx, &sr->body, parse_fail);
    irwriter_br(w, parse_success);

    // --- parse success ---
    irwriter_bb_at(w, parse_success);
    IrVal start_col = irwriter_call_retf(w, "i64", "top", "ptr %%r%d", (int)stack_ptr);
    IrVal cur_col = irwriter_load(w, "i64", col);
    IrVal parsed_tokens_val = irwriter_binop(w, "sub", "i64", cur_col, start_col);
    irwriter_store(w, "i64", parsed_tokens_val, parsed_tokens);

    if (tag_size > 0) {
      if (tag_size <= 64) {
        // Small: clear+OR single bucket
        uint64_t clear_mask = ~_tag_mask(tag_size, (uint64_t)sr->tag_bit_offset);
        irwriter_call_void_fmtf(w, "tag_writeback", "ptr %%r%d, i64 %%r%d, i64 %lld, i64 %lld, i64 %llu, i64 %%r%d",
                                (int)peg_table, (int)start_col, (long long)sizeof_col, (long long)sc.tag_byte_offset,
                                (unsigned long long)clear_mask, (int)irwriter_load(w, "i64", _tag_alloca(&ctx, 0)));
      } else {
        // Big: full overwrite of consecutive dedicated buckets
        int32_t n_buckets = (tag_size + 63) / 64;
        for (int32_t bi = 0; bi < n_buckets; bi++) {
          int64_t bucket_offset = sc.tag_byte_offset + bi * 8;
          irwriter_call_void_fmtf(w, "tag_writeback", "ptr %%r%d, i64 %%r%d, i64 %lld, i64 %lld, i64 0, i64 %%r%d",
                                  (int)peg_table, (int)start_col, (long long)sizeof_col, (long long)bucket_offset,
                                  (int)irwriter_load(w, "i64", _tag_alloca(&ctx, bi)));
        }
      }
    }
    memoize_write(&ctx, &sc, sr, start_col);
    irwriter_br(w, ml.done_bb);

    // --- parse fail ---
    irwriter_bb_at(w, parse_fail);
    IrVal fail_col = irwriter_call_retf(w, "i64", "top", "ptr %%r%d", (int)stack_ptr);
    irwriter_store(w, "i64", fail_col, col);
    memoize_deny(&ctx, &sc, sr, fail_col);
    irwriter_br(w, ml.fail_bb);

    // --- fail_bb ---
    irwriter_bb_at(w, ml.fail_bb);
    irwriter_store(w, "i64", irwriter_imm_int(w, -1), parsed_tokens);
    irwriter_br(w, ml.done_bb);

    // --- done_bb ---
    irwriter_bb_at(w, ml.done_bb);
    peg_ir_emit_ret(&ctx);
  }

  irwriter_bb_at(w, final_ret);
  IrVal final_result = irwriter_load(w, "i64", parse_result);
  IrVal final_col = irwriter_load(w, "i64", col);
  IrVal ret_with_result = irwriter_insertvalue(w, "{i64, i64}", -1, "i64", final_result, 0);
  IrVal ret_with_col = irwriter_insertvalue(w, "{i64, i64}", ret_with_result, "i64", final_col, 1);
  irwriter_ret(w, "{i64, i64}", ret_with_col);
  irwriter_define_end(w);
  XFREE(tag_bits_extra);
  XFREE(fn_name);
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
  peg_ir_emit_gep_helpers(w);
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
