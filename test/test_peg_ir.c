#include "../src/darray.h"
#include "../src/irwriter.h"
#include "../src/peg.h"
#include "../src/peg_ir.h"
#include "compat.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s...", #name);                                                                                          \
    name();                                                                                                            \
    printf(" OK\n");                                                                                                   \
  } while (0)

// helper: set up a minimal PegIrCtx
static PegIrCtx _make_ctx(IrWriter* w, ScopedRule* rules, int32_t n_rules) {
  PegIrCtx ctx = {0};
  ctx.w = w;
  ctx.scope_name = "test";
  ctx.rules = rules;
  ctx.compress = false;
  ctx.col_sizeof = n_rules * 4;
  ctx.bits_offset = 0;
  ctx.slots_offset = 0;
  ctx.n_seg_groups = 0;
  ctx.n_slots = n_rules;
  return ctx;
}

// ============================================================
// Test: peg_ir_read_slot / peg_ir_write_slot emit valid IR
// ============================================================

TEST(test_slot_access) {
  char* buf = NULL;
  size_t len = 0;
  FILE* f = compat_open_memstream(&buf, &len);
  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "test.c", ".");

  const char* at[] = {"ptr", "ptr", "i32"};
  const char* an[] = {"table", "tokens", "ntoks"};
  irwriter_define_start(w, "test_slot", "void", 3, at, an);
  irwriter_bb(w);

  ScopedRule rules[2] = {
      {.name = "r0", .kind = SCOPED_RULE_KIND_TERM, .as.term = 1, .slot_index = 0},
      {.name = "r1", .kind = SCOPED_RULE_KIND_TERM, .as.term = 2, .slot_index = 1},
  };

  PegIrCtx ctx = _make_ctx(w, rules, 2);
  ctx.table = irwriter_imm(w, "%table");
  ctx.tokens = irwriter_imm(w, "%tokens");
  ctx.n_tokens = irwriter_imm(w, "%ntoks");
  ctx.col_index = irwriter_alloca(w, "i32");
  ctx.stack = irwriter_alloca(w, "ptr");
  ctx.stack_bp = irwriter_alloca(w, "ptr");
  ctx.ret_val = irwriter_alloca(w, "i32");
  ctx.col_sizeof = 8; // 2 slots * 4 bytes

  IrVal col = irwriter_imm_int(w, 0);
  IrVal val = peg_ir_read_slot(&ctx, col, 0);
  assert(val >= 0); // valid register

  peg_ir_write_slot(&ctx, col, 1, irwriter_imm_int(w, 42));

  irwriter_ret_void(w);
  irwriter_define_end(w);
  irwriter_end(w);
  irwriter_del(w);
  fclose(f);

  assert(buf != NULL);
  assert(strstr(buf, "@table_gep") != NULL);
  assert(strstr(buf, "load") != NULL);
  assert(strstr(buf, "store") != NULL);

  free(buf);
}

// ============================================================
// Test: peg_ir_bit_test/deny/exclude emit correct instructions
// ============================================================

TEST(test_bit_ops) {
  char* buf = NULL;
  size_t len = 0;
  FILE* f = compat_open_memstream(&buf, &len);
  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "test.c", ".");

  const char* at[] = {"ptr"};
  const char* an[] = {"table"};
  irwriter_define_start(w, "test_bits", "void", 1, at, an);
  irwriter_bb(w);

  ScopedRule rules[1] = {{.name = "r0", .kind = SCOPED_RULE_KIND_TERM, .as.term = 1}};
  PegIrCtx ctx = _make_ctx(w, rules, 1);
  ctx.table = irwriter_imm(w, "%table");
  ctx.col_index = irwriter_alloca(w, "i32");
  ctx.col_sizeof = 8; // 1 seg_group (4 bytes) + 1 slot (4 bytes)
  ctx.bits_offset = 0;
  ctx.slots_offset = 4;
  ctx.compress = true;
  ctx.n_seg_groups = 1;
  ctx.n_slots = 1;

  IrVal col = irwriter_imm_int(w, 0);

  IrVal test_result = peg_ir_bit_test(&ctx, col, 0, 0x1);
  assert(test_result >= 0);

  peg_ir_bit_deny(&ctx, col, 0, 0x1);
  peg_ir_bit_exclude(&ctx, col, 0, 0x1);

  irwriter_ret_void(w);
  irwriter_define_end(w);
  irwriter_end(w);
  irwriter_del(w);
  fclose(f);

  // verify IR contains calls to bit helpers
  assert(strstr(buf, "@bit_test") != NULL);
  assert(strstr(buf, "@bit_deny") != NULL);
  assert(strstr(buf, "@bit_exclude") != NULL);

  free(buf);
}

// ============================================================
// Test: peg_ir_term emits bounds check + match
// ============================================================

TEST(test_term) {
  char* buf = NULL;
  size_t len = 0;
  FILE* f = compat_open_memstream(&buf, &len);
  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "test.c", ".");

  const char* at[] = {"ptr", "i32"};
  const char* an[] = {"tokens", "ntoks"};
  irwriter_define_start(w, "test_term", "void", 2, at, an);
  irwriter_bb(w);

  ScopedRule rules[1] = {{.name = "r0", .kind = SCOPED_RULE_KIND_TERM, .as.term = 5}};
  PegIrCtx ctx = _make_ctx(w, rules, 1);
  ctx.tokens = irwriter_imm(w, "%tokens");
  ctx.n_tokens = irwriter_imm(w, "%ntoks");
  ctx.col_index = irwriter_alloca(w, "i32");
  irwriter_store(w, "i32", irwriter_imm_int(w, 0), ctx.col_index);
  ctx.table = irwriter_imm(w, "null");
  ctx.stack = irwriter_alloca(w, "ptr");
  ctx.stack_bp = irwriter_alloca(w, "ptr");
  ctx.ret_val = irwriter_alloca(w, "i32");

  int32_t fail_bb = irwriter_label(w);
  ctx.fail_label = fail_bb;

  IrVal r = peg_ir_term(&ctx, 5);
  assert(r != 0); // should be imm "1"

  irwriter_ret_void(w);

  irwriter_bb_at(w, fail_bb);
  irwriter_ret_void(w);

  irwriter_define_end(w);
  irwriter_end(w);
  irwriter_del(w);
  fclose(f);

  // check for bounds check and comparison
  assert(strstr(buf, "icmp slt") != NULL);
  assert(strstr(buf, "icmp eq") != NULL);

  free(buf);
}

// ============================================================
// Test: peg_ir_seq generates sequential matches
// ============================================================

TEST(test_seq) {
  char* buf = NULL;
  size_t len = 0;
  FILE* f = compat_open_memstream(&buf, &len);
  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "test.c", ".");

  const char* at[] = {"ptr", "i32"};
  const char* an[] = {"tokens", "ntoks"};
  irwriter_define_start(w, "test_seq", "void", 2, at, an);
  irwriter_bb(w);

  // two terminals in sequence
  ScopedRule rules[3] = {
      {.name = "t0", .kind = SCOPED_RULE_KIND_TERM, .as.term = 1},
      {.name = "t1", .kind = SCOPED_RULE_KIND_TERM, .as.term = 2},
      {.name = "seq", .kind = SCOPED_RULE_KIND_SEQ},
  };
  int32_t* seq_kids = darray_new(sizeof(int32_t), 0);
  int32_t v0 = 0, v1 = 1;
  darray_push(seq_kids, v0);
  darray_push(seq_kids, v1);
  rules[2].as.seq = seq_kids;

  PegIrCtx ctx = _make_ctx(w, rules, 3);
  ctx.tokens = irwriter_imm(w, "%tokens");
  ctx.n_tokens = irwriter_imm(w, "%ntoks");
  ctx.col_index = irwriter_alloca(w, "i32");
  irwriter_store(w, "i32", irwriter_imm_int(w, 0), ctx.col_index);
  ctx.table = irwriter_imm(w, "null");
  ctx.stack = irwriter_alloca(w, "ptr");
  ctx.stack_bp = irwriter_alloca(w, "ptr");
  ctx.ret_val = irwriter_alloca(w, "i32");

  int32_t fail_bb = irwriter_label(w);
  ctx.fail_label = fail_bb;

  IrVal r = peg_ir_seq(&ctx, rules[2].as.seq);
  (void)r;
  irwriter_ret_void(w);

  irwriter_bb_at(w, fail_bb);
  irwriter_ret_void(w);

  irwriter_define_end(w);
  irwriter_end(w);
  irwriter_del(w);
  fclose(f);

  // seq should have two icmp eq checks (one for each terminal)
  int count = 0;
  const char* p = buf;
  while ((p = strstr(p, "icmp eq")) != NULL) {
    count++;
    p++;
  }
  assert(count >= 2);

  free(buf);
  darray_del(seq_kids);
}

// ============================================================
// Test: peg_ir_choice generates save/restore pattern
// ============================================================

TEST(test_choice) {
  char* buf = NULL;
  size_t len = 0;
  FILE* f = compat_open_memstream(&buf, &len);
  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "test.c", ".");

  const char* at[] = {"ptr", "i32", "ptr"};
  const char* an[] = {"tokens", "ntoks", "stack"};
  irwriter_define_start(w, "test_choice", "void", 3, at, an);
  irwriter_bb(w);

  ScopedRule rules[2] = {
      {.name = "a", .kind = SCOPED_RULE_KIND_TERM, .as.term = 1},
      {.name = "b", .kind = SCOPED_RULE_KIND_TERM, .as.term = 2},
  };
  int32_t* branches = darray_new(sizeof(int32_t), 0);
  int32_t b0 = 0, b1 = 1;
  darray_push(branches, b0);
  darray_push(branches, b1);

  PegIrCtx ctx = _make_ctx(w, rules, 2);
  ctx.tokens = irwriter_imm(w, "%tokens");
  ctx.n_tokens = irwriter_imm(w, "%ntoks");
  ctx.col_index = irwriter_alloca(w, "i32");
  irwriter_store(w, "i32", irwriter_imm_int(w, 0), ctx.col_index);
  ctx.table = irwriter_imm(w, "null");
  ctx.stack = irwriter_alloca(w, "ptr");
  irwriter_store(w, "ptr", irwriter_imm(w, "%stack"), ctx.stack);
  ctx.stack_bp = irwriter_alloca(w, "ptr");
  ctx.ret_val = irwriter_alloca(w, "i32");

  int32_t fail_bb = irwriter_label(w);
  ctx.fail_label = fail_bb;

  IrVal r = peg_ir_choice(&ctx, branches);
  (void)r;
  irwriter_ret_void(w);

  irwriter_bb_at(w, fail_bb);
  irwriter_ret_void(w);

  irwriter_define_end(w);
  irwriter_end(w);
  irwriter_del(w);
  fclose(f);

  // choice should have stack save/restore calls
  assert(strstr(buf, "@save") != NULL);
  // should have two terminal checks
  int count = 0;
  const char* p = buf;
  while ((p = strstr(p, "icmp eq")) != NULL) {
    count++;
    p++;
  }
  assert(count >= 2);

  free(buf);
  darray_del(branches);
}

// ============================================================
// Test: peg_ir_maybe generates optional pattern
// ============================================================

TEST(test_maybe) {
  char* buf = NULL;
  size_t len = 0;
  FILE* f = compat_open_memstream(&buf, &len);
  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "test.c", ".");

  const char* at[] = {"ptr", "i32"};
  const char* an[] = {"tokens", "ntoks"};
  irwriter_define_start(w, "test_maybe", "void", 2, at, an);
  irwriter_bb(w);

  ScopedRule rules[1] = {
      {.name = "opt", .kind = SCOPED_RULE_KIND_TERM, .as.term = 1, .multiplier = '?'},
  };

  PegIrCtx ctx = _make_ctx(w, rules, 1);
  ctx.tokens = irwriter_imm(w, "%tokens");
  ctx.n_tokens = irwriter_imm(w, "%ntoks");
  ctx.col_index = irwriter_alloca(w, "i32");
  irwriter_store(w, "i32", irwriter_imm_int(w, 0), ctx.col_index);
  ctx.table = irwriter_imm(w, "null");
  ctx.stack = irwriter_alloca(w, "ptr");
  ctx.stack_bp = irwriter_alloca(w, "ptr");
  ctx.ret_val = irwriter_alloca(w, "i32");

  int32_t fail_bb = irwriter_label(w);
  ctx.fail_label = fail_bb;

  // maybe should always succeed (never go to fail)
  IrVal r = peg_ir_maybe(&ctx, SCOPED_RULE_KIND_TERM, 0);
  (void)r;
  irwriter_ret_void(w);

  irwriter_bb_at(w, fail_bb);
  irwriter_ret_void(w);

  irwriter_define_end(w);
  irwriter_end(w);
  irwriter_del(w);
  fclose(f);

  // should have a store 0 (for the miss case) and a branch pattern
  assert(strstr(buf, "store i32 0") != NULL || strstr(buf, "0,") != NULL);

  free(buf);
}

// ============================================================
// Finding 7: peg_ir_plus must restore col_index to its original
// value before returning. The caller (seq) advances col.
//
// Test: call peg_ir_plus directly, check that between marked
// comments the code includes a store that restores col to origin.
// ============================================================

TEST(test_seq_with_plus_child) {
  char* buf = NULL;
  size_t len = 0;
  FILE* f = compat_open_memstream(&buf, &len);
  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "test.c", ".");

  const char* at[] = {"ptr", "i32"};
  const char* an[] = {"tokens", "ntoks"};
  irwriter_define_start(w, "test_plus_restore", "void", 2, at, an);
  irwriter_bb(w);

  ScopedRule rules[1] = {
      {.name = "elem", .kind = SCOPED_RULE_KIND_TERM, .as.term = 1},
  };

  PegIrCtx ctx = _make_ctx(w, rules, 1);
  ctx.tokens = irwriter_imm(w, "%tokens");
  ctx.n_tokens = irwriter_imm(w, "%ntoks");
  ctx.col_index = irwriter_alloca(w, "i32");
  irwriter_store(w, "i32", irwriter_imm_int(w, 0), ctx.col_index);
  ctx.table = irwriter_imm(w, "null");
  ctx.stack = irwriter_alloca(w, "ptr");
  ctx.stack_bp = irwriter_alloca(w, "ptr");
  ctx.ret_val = irwriter_alloca(w, "i32");

  int32_t fail_bb = irwriter_label(w);
  ctx.fail_label = fail_bb;

  irwriter_comment(w, "PLUS_BEGIN");
  IrVal result = peg_ir_plus(&ctx, SCOPED_RULE_KIND_TERM, 0, 0, 0);
  (void)result;
  irwriter_comment(w, "PLUS_END");

  irwriter_ret_void(w);
  irwriter_bb_at(w, fail_bb);
  irwriter_ret_void(w);

  irwriter_define_end(w);
  irwriter_end(w);
  irwriter_del(w);
  fclose(f);

  // Between PLUS_BEGIN and PLUS_END, plus must restore col to its
  // original value. The original value was loaded before plus started
  // and stored in col_origin. At the end, plus does:
  //   _store_col(ctx, col_origin)
  // Without the fix, plus never restores col.
  //
  // We detect the restore by counting "store i32" between the markers.
  // A correct plus has: col_save init(1), loop col_save(1), col_save
  // restore(1), origin restore(1) = 4+ stores.
  // A buggy plus without origin restore has fewer stores.

  const char* begin = strstr(buf, "PLUS_BEGIN");
  assert(begin != NULL);
  const char* end = strstr(buf, "PLUS_END");
  assert(end != NULL);

  int store_count = 0;
  const char* p = begin;
  while (p < end && (p = strstr(p, "store i32")) != NULL && p < end) {
    store_count++;
    p += 9;
  }
  // With fix: stores include col_save(1), loop col_save(1),
  // loop-exit col_save restore(1), origin restore(1), acc stores(2+) = 5+
  // Without fix: no origin restore, so fewer stores
  assert(store_count >= 5 && "plus must restore col_index to origin before returning");

  free(buf);
}

// ============================================================
// Finding 7: peg_ir_star must restore col_index to its original
// value before returning. The caller (seq) advances col.
// ============================================================

TEST(test_star_does_not_own_col) {
  char* buf = NULL;
  size_t len = 0;
  FILE* f = compat_open_memstream(&buf, &len);
  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "test.c", ".");

  const char* at[] = {"ptr", "i32"};
  const char* an[] = {"tokens", "ntoks"};
  irwriter_define_start(w, "test_star_restore", "void", 2, at, an);
  irwriter_bb(w);

  ScopedRule rules[1] = {
      {.name = "elem", .kind = SCOPED_RULE_KIND_TERM, .as.term = 1},
  };

  PegIrCtx ctx = _make_ctx(w, rules, 1);
  ctx.tokens = irwriter_imm(w, "%tokens");
  ctx.n_tokens = irwriter_imm(w, "%ntoks");
  ctx.col_index = irwriter_alloca(w, "i32");
  irwriter_store(w, "i32", irwriter_imm_int(w, 0), ctx.col_index);
  ctx.table = irwriter_imm(w, "null");
  ctx.stack = irwriter_alloca(w, "ptr");
  ctx.stack_bp = irwriter_alloca(w, "ptr");
  ctx.ret_val = irwriter_alloca(w, "i32");

  int32_t fail_bb = irwriter_label(w);
  ctx.fail_label = fail_bb;

  irwriter_comment(w, "STAR_BEGIN");
  IrVal result = peg_ir_star(&ctx, SCOPED_RULE_KIND_TERM, 0, 0, 0);
  (void)result;
  irwriter_comment(w, "STAR_END");

  irwriter_ret_void(w);
  irwriter_bb_at(w, fail_bb);
  irwriter_ret_void(w);

  irwriter_define_end(w);
  irwriter_end(w);
  irwriter_del(w);
  fclose(f);

  const char* begin = strstr(buf, "STAR_BEGIN");
  assert(begin != NULL);
  const char* end = strstr(buf, "STAR_END");
  assert(end != NULL);

  // Star must restore col to origin at the end.
  // Count "store i32" between markers.
  // With fix: col_save(1), loop col_save restore(1), origin restore(1),
  //   acc stores(2+) = 4+
  // Without fix: no origin restore, fewer stores.
  int store_count = 0;
  const char* p = begin;
  while (p < end && (p = strstr(p, "store i32")) != NULL && p < end) {
    store_count++;
    p += 9;
  }
  assert(store_count >= 4 && "star must restore col_index to origin before returning");

  free(buf);
}

int main(void) {
  printf("test_peg_ir:\n");
  RUN(test_slot_access);
  RUN(test_bit_ops);
  RUN(test_term);
  RUN(test_seq);
  RUN(test_choice);
  RUN(test_maybe);
  RUN(test_seq_with_plus_child);
  RUN(test_star_does_not_own_col);
  printf("test_peg_ir: OK\n");
  return 0;
}
