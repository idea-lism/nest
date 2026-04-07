#include "../src/darray.h"
#include "../src/re_ir.h"
#include "../src/ustr.h"
#include "compat.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

// Run fn in a forked child; return true if child crashed (signal death).
static bool crashes(void (*fn)(void)) {
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid == 0) {
    fclose(stderr);
    fn();
    _exit(0);
  }
  int status;
  waitpid(pid, &status, 0);
  return WIFSIGNALED(status);
}

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
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0);
  assert(darray_size(ir) == 1);
  assert(ir[0].kind == RE_IR_LPAREN);
  assert(ir[0].start == 0);
  assert(ir[0].end == 0);
  re_ir_free(ir);
}

TEST(test_emit_multiple) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'a');
  ir = re_ir_emit(ir, RE_IR_FORK, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'b', 'b');
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
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
  ir = re_ir_emit(ir, RE_IR_APPEND_HEX, 0x1F600, 0x1F600);
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
  ir = re_ir_emit_ch(ir, LEX_CP_BOF);
  ir = re_ir_emit_ch(ir, LEX_CP_EOF);
  assert(darray_size(ir) == 2);
  assert(ir[0].start == LEX_CP_BOF);
  assert(ir[1].start == LEX_CP_EOF);
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
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'a');
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0);

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
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'a');

  ReIr c = re_ir_clone(ir);
  c = re_ir_emit(c, RE_IR_APPEND_CH, 'b', 'b');

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
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'z');
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, '0', '9');
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
  assert(darray_size(ir) == 4);
  assert(ir[0].kind == RE_IR_RANGE_BEGIN);
  assert(ir[3].kind == RE_IR_RANGE_END);
  re_ir_free(ir);
}

TEST(test_range_neg_ic) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'z');
  ir = re_ir_emit(ir, RE_IR_RANGE_NEG, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_IC, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
  assert(darray_size(ir) == 5);
  assert(ir[2].kind == RE_IR_RANGE_NEG);
  assert(ir[3].kind == RE_IR_RANGE_IC);
  re_ir_free(ir);
}

TEST(test_paren_fork) {
  // (a|b)
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'a');
  ir = re_ir_emit(ir, RE_IR_FORK, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'b', 'b');
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
  assert(darray_size(ir) == 5);
  re_ir_free(ir);
}

TEST(test_nested_parens) {
  // ((a|b)c)
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'a');
  ir = re_ir_emit(ir, RE_IR_FORK, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'b', 'b');
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'c', 'c');
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
  assert(darray_size(ir) == 8);
  re_ir_free(ir);
}

TEST(test_action) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'a');
  ir = re_ir_emit(ir, RE_IR_ACTION, 42, 0);
  assert(darray_size(ir) == 2);
  assert(ir[1].kind == RE_IR_ACTION);
  assert(ir[1].start == 42);
  re_ir_free(ir);
}

TEST(test_groups) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_S, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_W, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_D, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_H, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_DOT, 0, 0);
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
  ir = re_ir_emit(ir, RE_IR_APPEND_C_ESCAPE, 'n', 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_C_ESCAPE, 't', 0);
  assert(darray_size(ir) == 2);
  assert(ir[0].kind == RE_IR_APPEND_C_ESCAPE);
  assert(ir[0].start == 'n');
  assert(ir[1].start == 't');
  re_ir_free(ir);
}

TEST(test_hex) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_APPEND_HEX, 0x41, 0x41);
  assert(darray_size(ir) == 1);
  assert(ir[0].kind == RE_IR_APPEND_HEX);
  assert(ir[0].start == 0x41);
  assert(ir[0].end == 0x41);
  re_ir_free(ir);
}

TEST(test_ignore_case) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_APPEND_CH_IC, 'A', 'A');
  assert(darray_size(ir) == 1);
  assert(ir[0].kind == RE_IR_APPEND_CH_IC);
  assert(ir[0].start == 'A');
  re_ir_free(ir);
}

// --- re_ir_exec ---

static char* _exec_to_ir(ReIr ir, DebugInfo di) {
  char* buf = NULL;
  size_t sz = 0;
  FILE* f = compat_open_memstream(&buf, &sz);
  assert(f);
  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "test.rules", ".");

  Aut* aut = aut_new("match", "test.rules");
  Re* re = re_new(aut);
  re_ir_exec(re, ir, di);
  re_action(re, 1);
  aut_gen_dfa(aut, w, false);
  re_del(re);
  aut_del(aut);

  irwriter_end(w);
  irwriter_del(w);
  compat_close_memstream(f, &buf, &sz);
  return buf;
}

TEST(test_exec_single_ch) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit_ch(ir, 'a');
  char* out = _exec_to_ir(ir, (DebugInfo){0, 0});
  re_ir_free(ir);

  assert(strstr(out, "i32 97")); // 'a'
  free(out);
}

TEST(test_exec_literal) {
  char* s = ustr_new(3, "abc");
  ReIr ir = re_ir_build_literal(s, 0, 3);
  char* out = _exec_to_ir(ir, (DebugInfo){0, 0});
  re_ir_free(ir);
  ustr_del(s);

  assert(strstr(out, "i32 97")); // 'a'
  assert(strstr(out, "i32 98")); // 'b'
  assert(strstr(out, "i32 99")); // 'c'
  free(out);
}

TEST(test_exec_range) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'z');
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, '0', '9');
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
  char* out = _exec_to_ir(ir, (DebugInfo){0, 0});
  re_ir_free(ir);

  assert(strstr(out, "icmp sge i32 %cp"));
  assert(strstr(out, "icmp sle i32 %cp"));
  free(out);
}

TEST(test_exec_range_neg) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'z');
  ir = re_ir_emit(ir, RE_IR_RANGE_NEG, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
  char* out = _exec_to_ir(ir, (DebugInfo){0, 0});
  re_ir_free(ir);

  assert(strstr(out, "icmp sge i32 %cp"));
  assert(strstr(out, "icmp sle i32 %cp"));
  free(out);
}

TEST(test_exec_paren_fork) {
  // (a|b)
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0);
  ir = re_ir_emit_ch(ir, 'a');
  ir = re_ir_emit(ir, RE_IR_FORK, 0, 0);
  ir = re_ir_emit_ch(ir, 'b');
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
  char* out = _exec_to_ir(ir, (DebugInfo){0, 0});
  re_ir_free(ir);

  assert(strstr(out, "i32 97")); // 'a'
  assert(strstr(out, "i32 98")); // 'b'
  free(out);
}

TEST(test_exec_group_d) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_D, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
  char* out = _exec_to_ir(ir, (DebugInfo){0, 0});
  re_ir_free(ir);

  // \d = [0-9], must produce range check for 48..57
  assert(strstr(out, "icmp sge i32 %cp"));
  assert(strstr(out, "icmp sle i32 %cp"));
  free(out);
}

TEST(test_exec_group_w) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_W, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
  char* out = _exec_to_ir(ir, (DebugInfo){0, 0});
  re_ir_free(ir);

  // \w = [0-9A-Z_a-z], must produce range checks
  assert(strstr(out, "icmp sge i32 %cp"));
  assert(strstr(out, "icmp sle i32 %cp"));
  free(out);
}

TEST(test_exec_group_s) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_S, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
  char* out = _exec_to_ir(ir, (DebugInfo){0, 0});
  re_ir_free(ir);

  // \s = [\t-\r ] = ranges [9..13] and [32..32]
  assert(strstr(out, "icmp sge i32 %cp"));
  assert(strstr(out, "icmp sle i32 %cp"));
  free(out);
}

TEST(test_exec_group_h) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_H, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
  char* out = _exec_to_ir(ir, (DebugInfo){0, 0});
  re_ir_free(ir);

  // \h = [0-9A-Fa-f], must produce range checks
  assert(strstr(out, "icmp sge i32 %cp"));
  assert(strstr(out, "icmp sle i32 %cp"));
  free(out);
}

TEST(test_exec_group_dot) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_DOT, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
  char* out = _exec_to_ir(ir, (DebugInfo){0, 0});
  re_ir_free(ir);

  // dot = [^\n] = [0..9] ++ [11..MAX_UNICODE], must produce range checks
  assert(strstr(out, "icmp sge i32 %cp"));
  assert(strstr(out, "icmp sle i32 %cp"));
  free(out);
}

TEST(test_exec_c_escape) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_APPEND_C_ESCAPE, 'n', 0);
  char* out = _exec_to_ir(ir, (DebugInfo){0, 0});
  re_ir_free(ir);

  assert(strstr(out, "i32 10")); // '\n'
  free(out);
}

TEST(test_exec_hex) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_APPEND_HEX, 0x41, 0x41);
  char* out = _exec_to_ir(ir, (DebugInfo){0, 0});
  re_ir_free(ir);

  assert(strstr(out, "i32 65")); // 0x41 = 'A'
  free(out);
}

TEST(test_exec_action) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit_ch(ir, 'a');
  ir = re_ir_emit(ir, RE_IR_ACTION, 42, 0);

  char* buf = NULL;
  size_t sz = 0;
  FILE* f = compat_open_memstream(&buf, &sz);
  assert(f);
  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "test.rules", ".");
  Aut* aut = aut_new("match", "test.rules");
  Re* re = re_new(aut);
  re_ir_exec(re, ir, (DebugInfo){0, 0});
  aut_gen_dfa(aut, w, false);
  re_del(re);
  aut_del(aut);
  irwriter_end(w);
  irwriter_del(w);
  compat_close_memstream(f, &buf, &sz);
  re_ir_free(ir);

  assert(strstr(buf, "i32 42, 1")); // action_id=42 emitted
  free(buf);
}

TEST(test_exec_ignore_case) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_APPEND_CH_IC, 'A', 'A');
  char* out = _exec_to_ir(ir, (DebugInfo){0, 0});
  re_ir_free(ir);

  assert(strstr(out, "i32 65")); // 'A'
  assert(strstr(out, "i32 97")); // 'a' — case-insensitive adds both
  free(out);
}

TEST(test_exec_range_ic) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'z');
  ir = re_ir_emit(ir, RE_IR_RANGE_IC, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
  char* out = _exec_to_ir(ir, (DebugInfo){0, 0});
  re_ir_free(ir);

  // [a-z] with ignore-case should produce ranges covering both a-z and A-Z
  assert(strstr(out, "icmp sge i32 %cp"));
  assert(strstr(out, "icmp sle i32 %cp"));
  free(out);
}

TEST(test_exec_debug_info_passthrough) {
  ReIr ir = re_ir_new();
  ir = re_ir_emit_ch(ir, 'x');
  char* out = _exec_to_ir(ir, (DebugInfo){10, 5});
  re_ir_free(ir);

  assert(strstr(out, "i32 120"));                        // 'x'
  assert(strstr(out, "DILocation(line: 10, column: 5")); // debug info passed through
  free(out);
}

TEST(test_exec_complex) {
  // (([a-z]|\d)\w) => action 1
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'z');
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
  ir = re_ir_emit(ir, RE_IR_FORK, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_D, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_W, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_ACTION, 1, 0);

  char* buf = NULL;
  size_t sz = 0;
  FILE* f = compat_open_memstream(&buf, &sz);
  assert(f);
  IrWriter* w = irwriter_new(f, NULL);
  irwriter_start(w, "test.rules", ".");
  Aut* aut = aut_new("match", "test.rules");
  Re* re = re_new(aut);
  re_ir_exec(re, ir, (DebugInfo){0, 0});
  aut_gen_dfa(aut, w, false);
  re_del(re);
  aut_del(aut);
  irwriter_end(w);
  irwriter_del(w);
  compat_close_memstream(f, &buf, &sz);
  re_ir_free(ir);

  assert(strstr(buf, "i32 1, 1"));         // action_id=1 emitted
  assert(strstr(buf, "icmp sge i32 %cp")); // ranges from \d and \w
  assert(strstr(buf, "icmp sle i32 %cp"));
  free(buf);
}

// --- Malformed IR (should crash/abort) ---

static void malformed_range_end_no_begin(void) {
  Aut* aut = aut_new("test", "test.re");
  Re* re = re_new(aut);
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
  re_ir_exec(re, ir, (DebugInfo){0, 0});
  re_ir_free(ir);
  re_del(re);
  aut_del(aut);
}

TEST(test_exec_malformed_range_end_no_begin) { assert(crashes(malformed_range_end_no_begin)); }

static void malformed_range_neg_no_begin(void) {
  Aut* aut = aut_new("test", "test.re");
  Re* re = re_new(aut);
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_RANGE_NEG, 0, 0);
  re_ir_exec(re, ir, (DebugInfo){0, 0});
  re_ir_free(ir);
  re_del(re);
  aut_del(aut);
}

TEST(test_exec_malformed_range_neg_no_begin) { assert(crashes(malformed_range_neg_no_begin)); }

static void malformed_range_ic_no_begin(void) {
  Aut* aut = aut_new("test", "test.re");
  Re* re = re_new(aut);
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_RANGE_IC, 0, 0);
  re_ir_exec(re, ir, (DebugInfo){0, 0});
  re_ir_free(ir);
  re_del(re);
  aut_del(aut);
}

TEST(test_exec_malformed_range_ic_no_begin) { assert(crashes(malformed_range_ic_no_begin)); }

static void malformed_nested_range_begin(void) {
  Aut* aut = aut_new("test", "test.re");
  Re* re = re_new(aut);
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  re_ir_exec(re, ir, (DebugInfo){0, 0});
  re_ir_free(ir);
  re_del(re);
  aut_del(aut);
}

TEST(test_exec_malformed_nested_range_begin) { assert(crashes(malformed_nested_range_begin)); }

static void malformed_unclosed_range(void) {
  Aut* aut = aut_new("test", "test.re");
  Re* re = re_new(aut);
  ReIr ir = re_ir_new();
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
  ir = re_ir_emit(ir, RE_IR_APPEND_CH, 'a', 'z');
  // missing RANGE_END
  re_ir_exec(re, ir, (DebugInfo){0, 0});
  re_ir_free(ir);
  re_del(re);
  aut_del(aut);
}

TEST(test_exec_malformed_unclosed_range) { assert(crashes(malformed_unclosed_range)); }

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

  // Exec (happy path)
  RUN(test_exec_single_ch);
  RUN(test_exec_literal);
  RUN(test_exec_range);
  RUN(test_exec_range_neg);
  RUN(test_exec_paren_fork);
  RUN(test_exec_group_d);
  RUN(test_exec_group_w);
  RUN(test_exec_group_s);
  RUN(test_exec_group_h);
  RUN(test_exec_group_dot);
  RUN(test_exec_c_escape);
  RUN(test_exec_hex);
  RUN(test_exec_action);
  RUN(test_exec_ignore_case);
  RUN(test_exec_range_ic);
  RUN(test_exec_debug_info_passthrough);
  RUN(test_exec_complex);

  // Malformed IR (should crash)
  RUN(test_exec_malformed_range_end_no_begin);
  RUN(test_exec_malformed_range_neg_no_begin);
  RUN(test_exec_malformed_range_ic_no_begin);
  RUN(test_exec_malformed_nested_range_begin);
  RUN(test_exec_malformed_unclosed_range);

  printf("all ok\n");
  return 0;
}
