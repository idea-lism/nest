#include "../src/header_writer.h"
#include "../src/irwriter.h"
#include "../src/parse.h"
#include "compat.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

void parse_nest(const char* src, HeaderWriter* header_writer, IrWriter* ir_writer);

#define TARGET "arm64-apple-macosx14.0.0"

// Minimal valid test source: one scope with tokens that match PEG usage.
// main scope emits @id and @assign (ignoring @comment and @space).
// PEG main rule uses exactly @id and @assign.
static const char* TEST_SRC = "[[vpa]]\n"
                              "\n"
                              "%ignore @comment @space\n"
                              "\n"
                              "%keyword ops \"=\" \"|\"\n"
                              "\n"
                              "main = {\n"
                              "  /[a-z_]\\w*/ @id\n"
                              "  /=/ @assign\n"
                              "  /#[^\\n]*/ @comment\n"
                              "  /[ \\t\\n]+/ @space\n"
                              "}\n"
                              "\n"
                              "[[peg]]\n"
                              "\n"
                              "main = @id @assign @id\n"
                              "\n";

// Extended test source with scopes, leader inlining, hooks, keywords, and macros.
static const char* TEST_SRC_SCOPED = "[[vpa]]\n"
                                     "\n"
                                     "%ignore @comment @space\n"
                                     "\n"
                                     "%keyword ops \"=\" \"|\"\n"
                                     "\n"
                                     "main = {\n"
                                     "  inner\n"
                                     "  /\\n+/ @nl\n"
                                     "  /#[^\\n]*/ @comment\n"
                                     "  /[ \\t\\n]+/ @space\n"
                                     "}\n"
                                     "\n"
                                     "inner = /\\{/ .begin {\n"
                                     "  /\\}/ .end\n"
                                     "  /[a-z_]\\w*/ @id\n"
                                     "  /=/ @assign\n"
                                     "  /#[^\\n]*/ @comment\n"
                                     "  /[ \\t\\n]+/ @space\n"
                                     "}\n"
                                     "\n"
                                     "[[peg]]\n"
                                     "\n"
                                     "main = @nl* inner+<@nl> @nl*\n"
                                     "\n"
                                     "inner = @id @assign @id\n"
                                     "\n";

static void _gen_output(const char* src, char** hdr_out, size_t* hdr_sz_out, char** ir_out, size_t* ir_sz_out) {
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  FILE* hdr_f = compat_open_memstream(&hdr_buf, &hdr_sz);
  assert(hdr_f);

  char* ir_buf = NULL;
  size_t ir_sz = 0;
  FILE* ir_f = compat_open_memstream(&ir_buf, &ir_sz);
  assert(ir_f);

  HeaderWriter* hw = hw_new(hdr_f);
  IrWriter* w = irwriter_new(ir_f, TARGET);
  irwriter_start(w, "test.nest", ".");

  parse_nest(src, hw, w);

  irwriter_end(w);
  irwriter_del(w);
  hw_del(hw);

  compat_close_memstream(hdr_f, &hdr_buf, &hdr_sz);
  compat_close_memstream(ir_f, &ir_buf, &ir_sz);

  *hdr_out = hdr_buf;
  *hdr_sz_out = hdr_sz;
  *ir_out = ir_buf;
  *ir_sz_out = ir_sz;
}

static bool _gen_succeeds(const char* src) {
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(src, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);
  bool ok = hdr_sz > 0 && ir_sz > 0;
  free(hdr_buf);
  free(ir_buf);
  return ok;
}

TEST(test_parse_basic) {
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(TEST_SRC, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);

  assert(hdr_buf != NULL);
  assert(hdr_sz > 0);
  assert(ir_buf != NULL);
  assert(ir_sz > 0);

  free(hdr_buf);
  free(ir_buf);
}

TEST(test_scoped) {
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(TEST_SRC_SCOPED, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);

  assert(hdr_buf != NULL);
  assert(hdr_sz > 0);
  assert(ir_buf != NULL);
  assert(ir_sz > 0);

  // DFA functions for scopes
  assert(strstr(ir_buf, "@lex_main("));
  assert(strstr(ir_buf, "@lex_inner("));

  // Scope IDs
  assert(strstr(hdr_buf, "SCOPE_MAIN"));
  assert(strstr(hdr_buf, "SCOPE_INNER"));

  // Dispatch and lex loop
  assert(strstr(ir_buf, "@vpa_dispatch("));
  assert(strstr(ir_buf, "@vpa_lex("));
  assert(strstr(ir_buf, "call void @vpa_rt_push_scope"));
  assert(strstr(ir_buf, "call void @vpa_rt_pop_scope"));
  assert(strstr(ir_buf, "call void @vpa_rt_emit_token"));

  // Token IDs
  assert(strstr(hdr_buf, "TOK_ID"));
  assert(strstr(hdr_buf, "TOK_ASSIGN"));
  assert(strstr(hdr_buf, "TOK_NL"));

  free(hdr_buf);
  free(ir_buf);
}

TEST(test_vpa_ir_compiles) {
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(TEST_SRC_SCOPED, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);

  // Truncate before PEG functions (PEG may have forward refs that fail standalone)
  char* peg_start = strstr(ir_buf, "define i32 @parse_");
  if (peg_start) {
    *peg_start = '\0';
  }

  char ll_path[256];
  char obj_path[256];
  snprintf(ll_path, sizeof(ll_path), "%s/test_vpa_compile.ll", BUILD_DIR);
  snprintf(obj_path, sizeof(obj_path), "%s/test_vpa_compile.o", BUILD_DIR);
  FILE* f = fopen(ll_path, "w");
  assert(f);
  fputs(ir_buf, f);
  fclose(f);

  char cmd[512];
  snprintf(cmd, sizeof(cmd), "%s -c -o %s %s 2>&1", compat_llvm_cc(), obj_path, ll_path);
  int rc = system(cmd);
  if (rc != 0) {
    fprintf(stderr, "clang compilation failed for %s\n", ll_path);
  }
  assert(rc == 0);

  remove(obj_path);
  remove(ll_path);
  free(hdr_buf);
  free(ir_buf);
}

// --- Token set validation tests ---

TEST(test_validate_peg_uses_missing_token) {
  // PEG uses @missing which VPA doesn't emit
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a @tok_b\n";
  assert(!_gen_succeeds(src));
}

TEST(test_validate_vpa_emits_unused_token) {
  // VPA emits @tok_b which PEG doesn't use
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "  /y/ @tok_b\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a\n";
  assert(!_gen_succeeds(src));
}

TEST(test_validate_matching_sets) {
  // VPA emit set == PEG used set
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "  /y/ @tok_b\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a @tok_b\n";
  assert(_gen_succeeds(src));
}

TEST(test_validate_ignored_tokens_excluded) {
  // @space is ignored so not required in PEG
  const char* src = "[[vpa]]\n"
                    "%ignore @space\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "  / +/ @space\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a\n";
  assert(_gen_succeeds(src));
}

TEST(test_validate_peg_subrule_tokens) {
  // PEG sub-rule uses @tok_b — still counts in main scope's used set
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "  /y/ @tok_b\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a sub\n"
                    "sub = @tok_b\n";
  assert(_gen_succeeds(src));
}

TEST(test_validate_scope_tokens_not_mixed) {
  // @inner_tok is emitted in inner scope, not in main
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  inner\n"
                    "  /x/ @tok_a\n"
                    "}\n"
                    "inner = /\\{/ .begin {\n"
                    "  /\\}/ .end\n"
                    "  /y/ @inner_tok\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a inner\n"
                    "inner = @inner_tok\n";
  assert(_gen_succeeds(src));
}

int main(void) {
  printf("test_parse:\n");

  RUN(test_parse_basic);
  RUN(test_scoped);
  RUN(test_vpa_ir_compiles);
  RUN(test_validate_peg_uses_missing_token);
  RUN(test_validate_vpa_emits_unused_token);
  RUN(test_validate_matching_sets);
  RUN(test_validate_ignored_tokens_excluded);
  RUN(test_validate_peg_subrule_tokens);
  RUN(test_validate_scope_tokens_not_mixed);

  printf("all ok\n");
  return 0;
}
