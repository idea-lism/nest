#include "../src/darray.h"
#include "../src/re_ir.h"
#include "../src/ustr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

// --- Creation / deletion ---

TEST(test_new_empty) {
  ReIr ir = re_ir_new();
  assert(ir != NULL);
  assert(darray_size(ir) == 0);
  re_ir_free(ir);
}

TEST(test_free_null) { re_ir_free(NULL); }

// --- Emit ---

TEST(test_emit_single) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0, 0, 0);
  assert(darray_size(ir) == 1);
  assert(ir[0].kind == RE_IR_LPAREN);
  assert(ir[0].start == 0);
  assert(ir[0].end == 0);
  re_ir_free(ir);
}

TEST(test_emit_multiple) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'a', 0, 0);
  ir = re_ir_emit(ir, RE_IR_FORK, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'b', 'b', 0, 0);
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0, 0, 0);
  assert(darray_size(ir) == 5);
  assert(ir[0].kind == RE_IR_LPAREN);
  assert(ir[1].kind == RE_IR_APPEND_CH);
  assert(ir[1].start == 'a');
  assert(ir[2].kind == RE_IR_FORK);
  assert(ir[3].kind == RE_IR_APPEND_CH);
  assert(ir[3].start == 'b');
  assert(ir[4].kind == RE_IR_RPAREN);
  re_ir_free(ir);
}

TEST(test_emit_start_end) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_APPEND_HEX, 0x1F600, 0x1F600, 0, 0);
  assert(darray_size(ir) == 1);
  assert(ir[0].kind == RE_IR_APPEND_HEX);
  assert(ir[0].start == 0x1F600);
  assert(ir[0].end == 0x1F600);
  re_ir_free(ir);
}

TEST(test_emit_ch) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit_ch(ir, 'x');
  assert(darray_size(ir) == 1);
  assert(ir[0].kind == RE_IR_APPEND_CH);
  assert(ir[0].start == 'x');
  re_ir_free(ir);
}

TEST(test_emit_ch_multibyte) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit_ch(ir, 0x20AC); // €
  assert(darray_size(ir) == 1);
  assert(ir[0].kind == RE_IR_APPEND_CH);
  assert(ir[0].start == 0x20AC);
  re_ir_free(ir);
}

TEST(test_emit_ch_special_codepoints) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit_ch(ir, 0);
  ir = re_ir_emit_ch(ir, 0x10FFFF);
  assert(darray_size(ir) == 2);
  assert(ir[0].start == 0);
  assert(ir[1].start == 0x10FFFF);
  re_ir_free(ir);
}

// --- Clone ---

TEST(test_clone_empty) {
  ReIr ir = re_ir_new();
  ReIr c = re_ir_clone(ir);
  assert(c != NULL);
  assert(c != ir);
  assert(darray_size(c) == 0);
  re_ir_free(ir);
  re_ir_free(c);
}

TEST(test_clone_preserves_ops) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'a', 0, 0);
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0, 0, 0);

  ReIr c = re_ir_clone(ir);
  assert(darray_size(c) == 3);
  for (size_t i = 0; i < 3; i++) {
    assert(c[i].kind == ir[i].kind);
    assert(c[i].start == ir[i].start);
    assert(c[i].end == ir[i].end);
  }
  re_ir_free(ir);
  re_ir_free(c);
}

TEST(test_clone_independence) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'a', 0, 0);

  ReIr c = re_ir_clone(ir);
  c = re_ir_emit(c, RE_IR_APPEND_CH, 'b', 'b', 0, 0);

  assert(darray_size(ir) == 1);
  assert(darray_size(c) == 2);
  assert(ir[0].start == 'a');
  assert(c[1].start == 'b');
  re_ir_free(ir);
  re_ir_free(c);
}

// --- Build literal ---

TEST(test_build_literal_ascii) {
  char* s = ustr_new(5, "hello");
  ReIr ir = re_ir_build_literal(s, 0, 5);
  assert(darray_size(ir) == 5);
  assert(ir[0].kind == RE_IR_APPEND_CH);
  assert(ir[0].start == 'h');
  assert(ir[1].start == 'e');
  assert(ir[2].start == 'l');
  assert(ir[3].start == 'l');
  assert(ir[4].start == 'o');
  re_ir_free(ir);
  ustr_del(s);
}

TEST(test_build_literal_multibyte) {
  // "café" = 63 61 66 C3 A9 — 4 codepoints
  char* s = ustr_new(5, "caf\xC3\xA9");
  ReIr ir = re_ir_build_literal(s, 0, 4);
  assert(darray_size(ir) == 4);
  assert(ir[0].start == 'c');
  assert(ir[1].start == 'a');
  assert(ir[2].start == 'f');
  assert(ir[3].start == 0xE9);
  re_ir_free(ir);
  ustr_del(s);
}

TEST(test_build_literal_4byte) {
  // U+1F600 = F0 9F 98 80
  char* s = ustr_new(4, "\xF0\x9F\x98\x80");
  ReIr ir = re_ir_build_literal(s, 0, 1);
  assert(darray_size(ir) == 1);
  assert(ir[0].kind == RE_IR_APPEND_CH);
  assert(ir[0].start == 0x1F600);
  re_ir_free(ir);
  ustr_del(s);
}

TEST(test_build_literal_offset) {
  // "hello" — skip 2 codepoints, take 2
  char* s = ustr_new(5, "hello");
  ReIr ir = re_ir_build_literal(s, 2, 2);
  assert(darray_size(ir) == 2);
  assert(ir[0].start == 'l');
  assert(ir[1].start == 'l');
  re_ir_free(ir);
  ustr_del(s);
}

TEST(test_build_literal_offset_multibyte) {
  // "aé€b" = 61 C3A9 E282AC 62 — skip 1, take 2
  char* s = ustr_new(7, "a\xC3\xA9\xE2\x82\xAC"
                        "b");
  ReIr ir = re_ir_build_literal(s, 1, 2);
  assert(darray_size(ir) == 2);
  assert(ir[0].start == 0xE9);
  assert(ir[1].start == 0x20AC);
  re_ir_free(ir);
  ustr_del(s);
}

TEST(test_build_literal_empty) {
  char* s = ustr_new(5, "hello");
  ReIr ir = re_ir_build_literal(s, 0, 0);
  assert(ir != NULL);
  assert(darray_size(ir) == 0);
  re_ir_free(ir);
  ustr_del(s);
}

// --- IR op sequences (valid patterns) ---

TEST(test_range_sequence) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'z', 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, '0', '9', 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0, 0, 0);
  assert(darray_size(ir) == 4);
  assert(ir[0].kind == RE_IR_RANGE_BEGIN);
  assert(ir[3].kind == RE_IR_RANGE_END);
  re_ir_free(ir);
}

TEST(test_range_neg_ic) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'z', 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_NEG, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_IC, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0, 0, 0);
  assert(darray_size(ir) == 5);
  assert(ir[2].kind == RE_IR_RANGE_NEG);
  assert(ir[3].kind == RE_IR_RANGE_IC);
  re_ir_free(ir);
}

TEST(test_paren_fork) {
  // (a|b)
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'a', 0, 0);
  ir = re_ir_emit(ir, RE_IR_FORK, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'b', 'b', 0, 0);
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0, 0, 0);
  assert(darray_size(ir) == 5);
  re_ir_free(ir);
}

TEST(test_nested_parens) {
  // ((a|b)c)
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'a', 0, 0);
  ir = re_ir_emit(ir, RE_IR_FORK, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'b', 'b', 0, 0);
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'c', 'c', 0, 0);
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0, 0, 0);
  assert(darray_size(ir) == 8);
  re_ir_free(ir);
}

TEST(test_action) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'a', 0, 0);
  ir = re_ir_emit(ir, RE_IR_ACTION, 42, 0, 0, 0);
  assert(darray_size(ir) == 2);
  assert(ir[1].kind == RE_IR_ACTION);
  assert(ir[1].start == 42);
  re_ir_free(ir);
}

TEST(test_groups) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_S, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_W, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_D, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_H, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_DOT, 0, 0, 0, 0);
  assert(darray_size(ir) == 5);
  assert(ir[0].kind == RE_IR_APPEND_GROUP_S);
  assert(ir[1].kind == RE_IR_APPEND_GROUP_W);
  assert(ir[2].kind == RE_IR_APPEND_GROUP_D);
  assert(ir[3].kind == RE_IR_APPEND_GROUP_H);
  assert(ir[4].kind == RE_IR_APPEND_GROUP_DOT);
  re_ir_free(ir);
}

TEST(test_c_escape) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_APPEND_C_ESCAPE, 'n', 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_C_ESCAPE, 't', 0, 0, 0);
  assert(darray_size(ir) == 2);
  assert(ir[0].kind == RE_IR_APPEND_C_ESCAPE);
  assert(ir[0].start == 'n');
  assert(ir[1].start == 't');
  re_ir_free(ir);
}

TEST(test_hex) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_APPEND_HEX, 0x41, 0x41, 0, 0);
  assert(darray_size(ir) == 1);
  assert(ir[0].kind == RE_IR_APPEND_HEX);
  assert(ir[0].start == 0x41);
  assert(ir[0].end == 0x41);
  re_ir_free(ir);
}

TEST(test_ignore_case) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_APPEND_CH_IC, 'A', 'A', 0, 0);
  assert(darray_size(ir) == 1);
  assert(ir[0].kind == RE_IR_APPEND_CH_IC);
  assert(ir[0].start == 'A');
  re_ir_free(ir);
}

// --- re_ir_validate ---

TEST(test_validate_ok_single_ch) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit_ch(ir, 'a');
  ReIrValidateResult res = re_ir_validate(ir, NULL);
  assert(res.err_type == RE_IR_OK);
  re_ir_free(ir);
}

TEST(test_validate_ok_range) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'z', 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0, 0, 0);
  ReIrValidateResult res = re_ir_validate(ir, NULL);
  assert(res.err_type == RE_IR_OK);
  re_ir_free(ir);
}

TEST(test_validate_ok_parens) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0, 0, 0);
  ir = re_ir_emit_ch(ir, 'a');
  ir = re_ir_emit(ir, RE_IR_FORK, 0, 0, 0, 0);
  ir = re_ir_emit_ch(ir, 'b');
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0, 0, 0);
  ReIrValidateResult res = re_ir_validate(ir, NULL);
  assert(res.err_type == RE_IR_OK);
  re_ir_free(ir);
}

TEST(test_validate_ok_nested_parens) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0, 0, 0);
  ir = re_ir_emit_ch(ir, 'a');
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0, 0, 0);
  ReIrValidateResult res = re_ir_validate(ir, NULL);
  assert(res.err_type == RE_IR_OK);
  re_ir_free(ir);
}

TEST(test_validate_ok_all_ops) {
  // exercise all op kinds in valid sequence
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'z', 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH_IC, 'A', 'A', 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_S, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_W, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_D, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_H, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_DOT, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_NEG, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_IC, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_FORK, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_C_ESCAPE, 'n', 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_HEX, 0x41, 0x41, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_LOOP_BACK, 0, 0, 0, 0);
  ir = re_ir_emit(ir, RE_IR_ACTION, 1, 0, 0, 0);
  ReIrValidateResult res = re_ir_validate(ir, NULL);
  assert(res.err_type == RE_IR_OK);
  re_ir_free(ir);
}

TEST(test_validate_empty_ir) {
  ReIr ir = re_ir_new();
  ReIrValidateResult res = re_ir_validate(ir, NULL);
  assert(res.err_type == RE_IR_OK);
  re_ir_free(ir);
}

TEST(test_validate_null_ir) {
  ReIrValidateResult res = re_ir_validate(NULL, NULL);
  assert(res.err_type == RE_IR_OK);
}

TEST(test_validate_frag_missing) {
  ReFrags frags = darray_new(sizeof(ReIr), 0);
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_FRAG_REF, 0, 0, 5, 3);
  ReIrValidateResult res = re_ir_validate(ir, frags);
  assert(res.err_type == RE_IR_ERR_MISSING_FRAG_ID);
  assert(res.frag_id == 0);
  assert(res.line == 5);
  assert(res.col == 3);
  re_ir_free(ir);
  darray_del(frags);
}

TEST(test_validate_frag_null_frags) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_FRAG_REF, 0, 0, 0, 0);
  ReIrValidateResult res = re_ir_validate(ir, NULL);
  assert(res.err_type == RE_IR_ERR_MISSING_FRAG_ID);
  assert(res.frag_id == 0);
  re_ir_free(ir);
}

TEST(test_validate_frag_recursion) {
  ReIr frag0 = re_ir_new();
  frag0 = re_ir_emit(frag0, RE_IR_FRAG_REF, 0, 0, 10, 1);
  ReFrags frags = darray_new(sizeof(ReIr), 0);
  darray_push(frags, frag0);

  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_FRAG_REF, 0, 0, 0, 0);
  ReIrValidateResult res = re_ir_validate(ir, frags);
  assert(res.err_type == RE_IR_ERR_RECURSION);
  assert(res.line == 10);
  assert(res.col == 1);
  re_ir_free(ir);
  re_ir_free(frag0);
  darray_del(frags);
}

TEST(test_validate_frag_mutual_recursion) {
  ReIr frag0 = re_ir_new();
  frag0 = re_ir_emit(frag0, RE_IR_FRAG_REF, 1, 0, 0, 0);
  ReIr frag1 = re_ir_new();
  frag1 = re_ir_emit(frag1, RE_IR_FRAG_REF, 0, 0, 7, 2);
  ReFrags frags = darray_new(sizeof(ReIr), 0);
  darray_push(frags, frag0);
  darray_push(frags, frag1);

  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_FRAG_REF, 0, 0, 0, 0);
  ReIrValidateResult res = re_ir_validate(ir, frags);
  assert(res.err_type == RE_IR_ERR_RECURSION);
  re_ir_free(ir);
  re_ir_free(frag0);
  re_ir_free(frag1);
  darray_del(frags);
}

TEST(test_validate_frag_ok) {
  ReIr frag0 = re_ir_new();
  frag0 = re_ir_emit(frag0, RE_IR_RANGE_BEGIN, 0, 0, 0, 0);
  frag0 = re_ir_emit(frag0, RE_IR_APPEND_CH, 'a', 'z', 0, 0);
  frag0 = re_ir_emit(frag0, RE_IR_RANGE_END, 0, 0, 0, 0);
  ReFrags frags = darray_new(sizeof(ReIr), 0);
  darray_push(frags, frag0);

  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_FRAG_REF, 0, 0, 0, 0);
  ReIrValidateResult res = re_ir_validate(ir, frags);
  assert(res.err_type == RE_IR_OK);
  re_ir_free(ir);
  re_ir_free(frag0);
  darray_del(frags);
}

TEST(test_validate_frag_chain_ok) {
  ReIr frag0 = re_ir_new();
  frag0 = re_ir_emit_ch(frag0, 'x');
  ReIr frag1 = re_ir_new();
  frag1 = re_ir_emit(frag1, RE_IR_FRAG_REF, 0, 0, 0, 0);
  frag1 = re_ir_emit_ch(frag1, 'y');
  ReFrags frags = darray_new(sizeof(ReIr), 0);
  darray_push(frags, frag0);
  darray_push(frags, frag1);

  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_FRAG_REF, 1, 0, 0, 0);
  ReIrValidateResult res = re_ir_validate(ir, frags);
  assert(res.err_type == RE_IR_OK);
  re_ir_free(ir);
  re_ir_free(frag0);
  re_ir_free(frag1);
  darray_del(frags);
}

int main(void) {
  printf("test_re_ir:\n");

  // Creation / deletion
  RUN(test_new_empty);
  RUN(test_free_null);

  // Emit
  RUN(test_emit_single);
  RUN(test_emit_multiple);
  RUN(test_emit_start_end);
  RUN(test_emit_ch);
  RUN(test_emit_ch_multibyte);
  RUN(test_emit_ch_special_codepoints);

  // Clone
  RUN(test_clone_empty);
  RUN(test_clone_preserves_ops);
  RUN(test_clone_independence);

  // Build literal
  RUN(test_build_literal_ascii);
  RUN(test_build_literal_multibyte);
  RUN(test_build_literal_4byte);
  RUN(test_build_literal_offset);
  RUN(test_build_literal_offset_multibyte);
  RUN(test_build_literal_empty);

  // IR op sequences
  RUN(test_range_sequence);
  RUN(test_range_neg_ic);
  RUN(test_paren_fork);
  RUN(test_nested_parens);
  RUN(test_action);
  RUN(test_groups);
  RUN(test_c_escape);
  RUN(test_hex);
  RUN(test_ignore_case);

  // Validate (structural checks)
  RUN(test_validate_ok_single_ch);
  RUN(test_validate_ok_range);
  RUN(test_validate_ok_parens);
  RUN(test_validate_ok_nested_parens);
  RUN(test_validate_ok_all_ops);
  RUN(test_validate_empty_ir);
  RUN(test_validate_null_ir);
  RUN(test_validate_frag_missing);
  RUN(test_validate_frag_null_frags);
  RUN(test_validate_frag_recursion);
  RUN(test_validate_frag_mutual_recursion);
  RUN(test_validate_frag_ok);
  RUN(test_validate_frag_chain_ok);

  printf("all ok\n");
  return 0;
}
