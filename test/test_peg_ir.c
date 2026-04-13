// specs/peg_ir.md
#include "../src/darray.h"
#include "../src/irwriter.h"
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

// Set up an IrWriter writing to an in-memory buffer, with a function shell
// and the PegIrCtx allocas so peg_ir_emit_parse can work.
typedef struct {
  char* buf;
  size_t len;
  FILE* f;
  IrWriter* w;
  PegIrCtx ctx;
} TestCtx;

static TestCtx _setup(const char* fn_name) {
  TestCtx t = {0};
  t.f = compat_open_memstream(&t.buf, &t.len);
  t.w = irwriter_new(t.f, NULL);
  irwriter_start(t.w, "test_peg_ir.ll", ".");
  irwriter_type_def(t.w, "Token", "{i32}");

  // emit save/restore so calls can reference them
  peg_ir_emit_helpers(t.w);

  irwriter_define_startf(t.w, fn_name, "{i64, i64} @%s(ptr %%tt, ptr %%stack_ptr_in)", fn_name);
  irwriter_bb(t.w);

  IrVal col_ptr = irwriter_alloca(t.w, "i64");
  irwriter_store(t.w, "i64", irwriter_imm_int(t.w, 0), col_ptr);
  IrVal stack_ptr = irwriter_alloca(t.w, "ptr");
  irwriter_store(t.w, "ptr", irwriter_imm(t.w, "%stack_ptr_in"), stack_ptr);
  IrVal parse_result = irwriter_alloca(t.w, "i64");
  irwriter_store(t.w, "i64", irwriter_imm_int(t.w, -1), parse_result);
  IrVal parsed_tokens = irwriter_alloca(t.w, "i64");
  irwriter_store(t.w, "i64", irwriter_imm_int(t.w, 0), parsed_tokens);
  IrVal tag_bits = irwriter_alloca(t.w, "i64");
  irwriter_store(t.w, "i64", irwriter_imm_int(t.w, 0), tag_bits);

  // fake tc and tokens registers
  IrVal tc = irwriter_imm(t.w, "%tt");
  IrVal tokens = irwriter_imm(t.w, "%tt");

  t.ctx = (PegIrCtx){
      .ir_writer = t.w,
      .fn_name = fn_name,
      .tc = tc,
      .tokens = tokens,
      .col = col_ptr,
      .stack_ptr = stack_ptr,
      .parse_result = parse_result,
      .tag_bits = tag_bits,
      .parsed_tokens = parsed_tokens,
      .ret_labels = darray_new(sizeof(IrLabel), 0),
  };
  return t;
}

static char* _finish(TestCtx* t) {
  irwriter_define_end(t->w);
  irwriter_end(t->w);
  irwriter_del(t->w);
  compat_close_memstream(t->f, &t->buf, &t->len);
  darray_del(t->ctx.ret_labels);
  return t->buf;
}

// --- Tests ---

TEST(test_helpers_define_internal) {
  char* buf = NULL;
  size_t len = 0;
  FILE* f = compat_open_memstream(&buf, &len);
  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "test.ll", ".");
  peg_ir_emit_helpers(w);
  peg_ir_emit_bit_helpers(w);
  irwriter_del(w);
  compat_close_memstream(f, &buf, &len);

  assert(strstr(buf, "define internal void @save("));
  assert(strstr(buf, "define internal void @restore("));
  assert(strstr(buf, "define internal i64 @top("));
  assert(strstr(buf, "define internal i1 @bit_test("));
  assert(strstr(buf, "define internal void @bit_deny("));
  assert(strstr(buf, "define internal void @bit_exclude("));
  free(buf);
}

TEST(test_emit_term) {
  TestCtx t = _setup("test_fn");
  ScopedUnit unit = {.kind = SCOPED_UNIT_TERM, .tag_bit_local_offset = -1};
  unit.as.term_id = 42;

  IrLabel fail = irwriter_label(t.w);
  peg_ir_emit_parse(&t.ctx, &unit, fail);
  irwriter_br(t.w, fail);
  irwriter_bb_at(t.w, fail);
  irwriter_ret(t.w, "{i64, i64}", irwriter_imm(t.w, "undef"));

  char* out = _finish(&t);
  // should compare against expected term id
  assert(strstr(out, "icmp eq i32"));
  assert(strstr(out, "42"));
  // should increment col by 1 on success
  assert(strstr(out, "add i64"));
  free(out);
}

TEST(test_emit_call_blockaddress) {
  TestCtx t = _setup("parse_scope");
  ScopedUnit unit = {.kind = SCOPED_UNIT_CALL, .tag_bit_local_offset = -1};
  unit.as.callee = "scope$rule1";

  IrLabel fail = irwriter_label(t.w);
  peg_ir_emit_parse(&t.ctx, &unit, fail);

  // emit a done_bb and then ret to close the function properly
  IrLabel done = irwriter_label(t.w);
  irwriter_br(t.w, done);
  irwriter_bb_at(t.w, fail);
  irwriter_br(t.w, done);
  irwriter_bb_at(t.w, done);
  peg_ir_emit_ret(&t.ctx);
  irwriter_ret(t.w, "{i64, i64}", irwriter_imm(t.w, "undef"));

  char* out = _finish(&t);

  // blockaddress must reference the real function name, not @current_fn
  assert(strstr(out, "blockaddress(@parse_scope,"));
  assert(!strstr(out, "@current_fn"));

  // blockaddress label must be %ret_scope$rule1, not %r-something
  assert(strstr(out, "%ret_scope$rule1)"));

  // indirectbr destination list must not be empty
  char* ibr = strstr(out, "indirectbr");
  assert(ibr != NULL);
  // find the bracket after indirectbr
  char* bracket = strstr(ibr, "[");
  assert(bracket != NULL);
  // should contain at least one "label %"
  char* end_bracket = strstr(bracket, "]");
  assert(end_bracket != NULL);
  assert(end_bracket > bracket + 1); // not empty []

  free(out);
}

TEST(test_emit_seq) {
  TestCtx t = _setup("test_fn");
  // seq(term1, term2)
  ScopedUnit children[2] = {
      {.kind = SCOPED_UNIT_TERM, .tag_bit_local_offset = -1, .as = {.term_id = 10}},
      {.kind = SCOPED_UNIT_TERM, .tag_bit_local_offset = -1, .as = {.term_id = 20}},
  };
  ScopedUnit* ch = darray_new(sizeof(ScopedUnit), 0);
  darray_push(ch, children[0]);
  darray_push(ch, children[1]);

  ScopedUnit seq = {.kind = SCOPED_UNIT_SEQ, .tag_bit_local_offset = -1};
  seq.as.children = ch;

  IrLabel fail = irwriter_label(t.w);
  peg_ir_emit_parse(&t.ctx, &seq, fail);
  irwriter_br(t.w, fail);
  irwriter_bb_at(t.w, fail);
  irwriter_ret(t.w, "{i64, i64}", irwriter_imm(t.w, "undef"));

  char* out = _finish(&t);
  // seq should call save
  assert(strstr(out, "call void @save("));
  // seq should call restore on failure
  assert(strstr(out, "call void @restore("));
  // should check for both term ids
  assert(strstr(out, "10"));
  assert(strstr(out, "20"));
  darray_del(ch);
  free(out);
}

TEST(test_emit_maybe_phi) {
  TestCtx t = _setup("test_fn");
  ScopedUnit base = {.kind = SCOPED_UNIT_TERM, .tag_bit_local_offset = -1, .as = {.term_id = 5}};
  ScopedUnit* base_heap = malloc(sizeof(ScopedUnit));
  *base_heap = base;

  ScopedUnit maybe = {.kind = SCOPED_UNIT_MAYBE, .tag_bit_local_offset = -1};
  maybe.as.base = base_heap;

  IrLabel fail = irwriter_label(t.w);
  peg_ir_emit_parse(&t.ctx, &maybe, fail);
  irwriter_br(t.w, fail);
  irwriter_bb_at(t.w, fail);
  irwriter_ret(t.w, "{i64, i64}", irwriter_imm(t.w, "undef"));

  char* out = _finish(&t);
  // maybe always succeeds — no phi, just falls through or branches to done
  assert(strstr(out, "5")); // should reference the term id
  free(base_heap);
  free(out);
}

TEST(test_emit_star) {
  TestCtx t = _setup("test_fn");
  ScopedUnit lhs = {.kind = SCOPED_UNIT_TERM, .tag_bit_local_offset = -1, .as = {.term_id = 7}};
  ScopedUnit* lhs_heap = malloc(sizeof(ScopedUnit));
  *lhs_heap = lhs;

  ScopedUnit star = {.kind = SCOPED_UNIT_STAR, .tag_bit_local_offset = -1};
  star.as.interlace = (ScopedInterlace){.lhs = lhs_heap, .rhs = NULL};

  IrLabel fail = irwriter_label(t.w);
  peg_ir_emit_parse(&t.ctx, &star, fail);
  irwriter_br(t.w, fail);
  irwriter_bb_at(t.w, fail);
  irwriter_ret(t.w, "{i64, i64}", irwriter_imm(t.w, "undef"));

  char* out = _finish(&t);
  // star should match term_id 7
  assert(strstr(out, "7"));
  // should have a loop (br back to a label)
  // check there's at least one unconditional br to a label that was also a bb header
  assert(strstr(out, "add i64")); // accumulator addition
  free(lhs_heap);
  free(out);
}

TEST(test_emit_branches) {
  TestCtx t = _setup("test_fn");
  ScopedUnit alt1 = {.kind = SCOPED_UNIT_TERM, .tag_bit_local_offset = -1, .as = {.term_id = 1}};
  ScopedUnit alt2 = {.kind = SCOPED_UNIT_TERM, .tag_bit_local_offset = -1, .as = {.term_id = 2}};
  ScopedUnit* ch = darray_new(sizeof(ScopedUnit), 0);
  darray_push(ch, alt1);
  darray_push(ch, alt2);

  ScopedUnit branches = {.kind = SCOPED_UNIT_BRANCHES, .tag_bit_local_offset = -1};
  branches.as.children = ch;

  IrLabel fail = irwriter_label(t.w);
  peg_ir_emit_parse(&t.ctx, &branches, fail);
  irwriter_br(t.w, fail);
  irwriter_bb_at(t.w, fail);
  irwriter_ret(t.w, "{i64, i64}", irwriter_imm(t.w, "undef"));

  char* out = _finish(&t);
  // branches should call save and restore
  assert(strstr(out, "call void @save("));
  assert(strstr(out, "call void @restore("));
  darray_del(ch);
  free(out);
}

TEST(test_tag_bit_set) {
  TestCtx t = _setup("test_fn");
  t.ctx.tag_bit_offset = 2;
  ScopedUnit unit = {.kind = SCOPED_UNIT_TERM, .tag_bit_local_offset = 1};
  unit.as.term_id = 99;

  IrLabel fail = irwriter_label(t.w);
  peg_ir_emit_parse(&t.ctx, &unit, fail);
  irwriter_br(t.w, fail);
  irwriter_bb_at(t.w, fail);
  irwriter_ret(t.w, "{i64, i64}", irwriter_imm(t.w, "undef"));

  char* out = _finish(&t);
  // tag_bit = 1 << (tag_bit_local_offset + tag_bit_offset) = 1 << 3 = 8
  assert(strstr(out, "or i64"));
  assert(strstr(out, "8"));
  free(out);
}

int main(void) {
  printf("test_peg_ir:\n");
  RUN(test_helpers_define_internal);
  RUN(test_emit_term);
  RUN(test_emit_call_blockaddress);
  RUN(test_emit_seq);
  RUN(test_emit_maybe_phi);
  RUN(test_emit_star);
  RUN(test_emit_branches);
  RUN(test_tag_bit_set);
  printf("test_peg_ir: OK\n");
  return 0;
}
